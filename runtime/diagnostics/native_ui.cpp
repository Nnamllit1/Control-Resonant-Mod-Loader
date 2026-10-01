#include "native_ui.h"
#include "../compatibility.h"
#include "../ui_service.h"
#include <Windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <vector>

extern "C" void crml_native_ui_forward0();
extern "C" void crml_native_ui_forward2();
extern "C" void crml_native_ui_forward3();
extern "C" void crml_native_ui_forward4();
extern "C" void crml_native_ui_forward6();
extern "C" void crml_native_ui_forward7();

namespace crml::native_ui {
namespace {
constexpr std::string_view html_sha="5d236f36c75f1765c9c8e454392ac9210dd37c8e97fd98c39deebd5b5d8f15c7";
constexpr std::string_view cohtml_sha="c026d81afd768a1734139fbfc00b3dfaaac72fba48642b0abe5f0818a6dfd2a6";
constexpr std::string_view marker="crml-native-ui-v1";
constexpr unsigned max_pending=4;
using Handler=void(*)(void*,void*,void*);
using GetBuffer=void*(*)(void*,size_t);
using Status=void(*)(void*,uint32_t);
using Finish=void(*)(void*,uint32_t);

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
public:
    Sha256() {
        if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0)
            BCryptCreateHash(algorithm_,&hash_,nullptr,0,nullptr,0,0);
    }
    ~Sha256() { if(hash_) BCryptDestroyHash(hash_); if(algorithm_) BCryptCloseAlgorithmProvider(algorithm_,0); }
    bool add(const void* bytes,size_t count) { return hash_ && count<=ULONG_MAX && BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(bytes)),static_cast<ULONG>(count),0)>=0; }
    std::string finish() {
        unsigned char value[32]{}; if(!hash_ || BCryptFinishHash(hash_,value,sizeof(value),0)<0) return {};
        constexpr char hex[]="0123456789abcdef"; std::string result(64,'0');
        for(size_t i=0;i<32;++i) { result[2*i]=hex[value[i]>>4]; result[2*i+1]=hex[value[i]&15]; }
        return result;
    }
};
std::string file_hash(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); if(!input) return {};
    Sha256 digest; std::array<char,65536> bytes{};
    while(input) { input.read(bytes.data(),bytes.size()); if(input.gcount() && !digest.add(bytes.data(),static_cast<size_t>(input.gcount()))) return {}; }
    return input.eof()?digest.finish():std::string{};
}
struct State {
    std::atomic<bool> enabled{};
    std::atomic<unsigned> pending{};
    std::atomic<uint64_t> requests{}, transformed{}, passthrough{}, failures{}, exhausted{}, completed{};
    std::atomic<uint64_t> observed{}, foreign_response{}, other_route{}, source_mismatch{};
    std::string payload;
    ui::Service* bridge{};
    std::string_view expected_html_sha{html_sha};
    Handler original{};
    uintptr_t module_begin{},module_end{};
    std::ofstream log;
    uint64_t last_report{};
    std::array<uint64_t,11> last_counts{};
};
State& state() { static auto* value=new State; return *value; }

struct Proxy {
    // Response methods are serialized by the reviewed native callback. Atomic
    // references protect deferred completion lifetime, not concurrent writes.
    void** table;
    void* original;
    std::atomic<uint8_t> bypass{};
    std::atomic<uint32_t> references{2}; // The handler call and its completion.
    std::atomic<bool> finished{};
    State* owner;
    std::unique_ptr<char[]> captured;
    size_t count{};
    uint32_t status{};
    bool allocation_failed{};
    Proxy(void** vtable,void* response,State& s):table(vtable),original(response),owner(&s) {}
    void release() noexcept { if(references.fetch_sub(1,std::memory_order_acq_rel)==1) { owner->pending.fetch_sub(1,std::memory_order_relaxed); delete this; } }
    template<class T> T method(size_t slot) const noexcept {return reinterpret_cast<T>((*static_cast<void***>(original))[slot]);}
    bool deliver(std::string_view bytes) {
        auto* buffer=method<GetBuffer>(1)(original,bytes.size());
        if(!buffer && !bytes.empty()) {allocation_failed=true; ++owner->failures; return false;}
        if(!bytes.empty()) std::memcpy(buffer,bytes.data(),bytes.size());
        return true;
    }
    bool flush() {
        if(!captured) return true;
        const bool ok=deliver({captured.get(),count});
        captured.reset(); count=0;
        return ok;
    }
};
static_assert(offsetof(Proxy,table)==0 && offsetof(Proxy,original)==8 && offsetof(Proxy,bypass)==16);

void* get_buffer(Proxy* proxy,size_t bytes) {
    if(proxy->finished.load(std::memory_order_acquire)) return nullptr;
    if(proxy->captured || !bytes || bytes>max_document || proxy->bypass.load(std::memory_order_relaxed)) {
        proxy->bypass=true;
        if(!proxy->flush()) return nullptr;
        return proxy->method<GetBuffer>(1)(proxy->original,bytes);
    }
    auto buffer=std::unique_ptr<char[]>(new(std::nothrow) char[bytes]{});
    if(!buffer) { proxy->bypass=true; return proxy->method<GetBuffer>(1)(proxy->original,bytes); }
    proxy->count=bytes;proxy->captured=std::move(buffer);
    return proxy->captured.get();
}
void set_status(Proxy* proxy,uint32_t value) {
    if(proxy->finished.load(std::memory_order_acquire)) return;
    proxy->status=value;
    proxy->method<Status>(5)(proxy->original,value);
}
void finish(Proxy* proxy,uint32_t result) {
    if(proxy->finished.exchange(true,std::memory_order_acq_rel)) return;
    struct Completion { Proxy* value; ~Completion(){value->release();} } completion{proxy};
    ++proxy->owner->completed;
    bool changed=false;
    if(proxy->captured) {
        std::string rewritten;
        if(!result && proxy->status==200 && !proxy->bypass.load(std::memory_order_relaxed)) {
            try {
                const std::string_view source(proxy->captured.get(),proxy->count);
                Sha256 digest;
                if(digest.add(source.data(),source.size()) && digest.finish()==proxy->owner->expected_html_sha) {
                    auto payload=proxy->owner->payload;
                    if(proxy->owner->bridge) {
                        constexpr std::string_view token="__CRML_UI_NONCE__";
                        const auto position=payload.find(token);
                        const auto page=position!=payload.npos?proxy->owner->bridge->open_page():0;
                        if(page) payload.replace(position,token.size(),std::to_string(page));
                        else payload.clear();
                    }
                    changed=insert_panel(source,payload,rewritten);
                }
                else ++proxy->owner->source_mismatch;
            } catch(const std::bad_alloc&) { ++proxy->owner->failures; }
        }
        const std::string_view bytes=changed?std::string_view(rewritten):std::string_view(proxy->captured.get(),proxy->count);
        if(!proxy->deliver(bytes)) { result=1; changed=false; }
    }
    if(proxy->allocation_failed) result=1;
    if(changed) ++proxy->owner->transformed; else ++proxy->owner->passthrough;
    // Finish may destroy the native response. Resolve the call before entering
    // it, then never inspect that response again.
    const auto native_finish=proxy->method<Finish>(8); void* original=proxy->original;
    native_finish(original,result);
}
void* proxy_table[]{reinterpret_cast<void*>(&crml_native_ui_forward0),reinterpret_cast<void*>(&get_buffer),
    reinterpret_cast<void*>(&crml_native_ui_forward2),reinterpret_cast<void*>(&crml_native_ui_forward3),
    reinterpret_cast<void*>(&crml_native_ui_forward4),reinterpret_cast<void*>(&set_status),
    reinterpret_cast<void*>(&crml_native_ui_forward6),reinterpret_cast<void*>(&crml_native_ui_forward7),reinterpret_cast<void*>(&finish)};

bool executable_pointer(uintptr_t pointer,const State& s) noexcept {
    if(pointer<s.module_begin || pointer>=s.module_end) return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(pointer),&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&PAGE_GUARD)) return false;
    return (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
}
bool readable_request(void* request,void* response,char (&url)[2048],State& s) noexcept {
    __try {
        auto** table=*static_cast<void***>(response);
        for(unsigned i=0;i<9;++i) if(!executable_pointer(reinterpret_cast<uintptr_t>(table[i]),s)) return false;
        const auto getter=(*static_cast<void***>(request))[2];
        if(!executable_pointer(reinterpret_cast<uintptr_t>(getter),s)) return false;
        const char* source=reinterpret_cast<const char*(*)(void*)>(getter)(request);
        if(!source) return false;
        for(size_t i=0;i<sizeof(url);++i) { url[i]=source[i]; if(!url[i]) return true; }
        return false;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool reserve(State& s) noexcept {
    auto value=s.pending.load(std::memory_order_relaxed);
    while(value<max_pending) if(s.pending.compare_exchange_weak(value,value+1,std::memory_order_acq_rel)) return true;
    ++s.exhausted; return false;
}
void intercept(State& s,void* handler,void* request,void* response,bool selected) {
    if(!selected || !reserve(s)) {s.original(handler,request,response);return;}
    auto* proxy=new(std::nothrow) Proxy(proxy_table,response,s);
    if(!proxy) {s.pending.fetch_sub(1,std::memory_order_relaxed);s.original(handler,request,response);return;}
    ++s.requests;
    struct Call {Proxy* proxy;~Call(){proxy->release();}} call{proxy};
    // Heap ownership remains with completion if Finish is deferred. Missing
    // completion can consume at most four proxy slots. This is lifetime support,
    // not authorization to hook an unreviewed callback on another game build.
    s.original(handler,request,proxy);
}
bool serve_ui(ui::Service& bridge,std::string_view url,void* response,uint64_t now) {
    if(!url.starts_with(ui::poll_prefix)) return false;
    std::string body;
    const auto code=bridge.exchange(url,now,body);
    auto** table=*static_cast<void***>(response);
    const auto allocate=reinterpret_cast<GetBuffer>(table[1]);
    const auto status=reinterpret_cast<Status>(table[5]);
    const auto complete=reinterpret_cast<Finish>(table[8]);
    status(response,static_cast<uint32_t>(code));
    auto* buffer=body.empty()?nullptr:allocate(response,body.size());
    if(buffer) std::memcpy(buffer,body.data(),body.size());
    complete(response,(!body.empty() && !buffer)?1u:0u);
    return true;
}
void hook(void* handler,void* request,void* response) {
    auto& s=state(); char url[2048]{};
    bool selected=false;
    if(s.enabled.load(std::memory_order_acquire)) {
        ++s.observed;
        if(!request || !response || !readable_request(request,response,url,s)) ++s.foreign_response;
        else if(s.bridge && serve_ui(*s.bridge,url,response,GetTickCount64())) return;
        else if(!target_url(url)) ++s.other_route;
        else selected=true;
    }
    intercept(s,handler,request,response,selected);
}
}

// Called by ABI-transparent assembly thunks. Only RCX changes in the forwarded
// call; unknown argument types and stack arguments never pass through C++.
extern "C" void* crml_native_ui_forward(void* address,uint32_t slot) {
    auto* proxy=static_cast<Proxy*>(address);void* original=proxy->original;
    proxy->bypass=true;
    if(slot==0) {
        if(!proxy->finished.exchange(true,std::memory_order_acq_rel)) {++proxy->owner->completed;proxy->release();}
    } else if(!proxy->finished.load(std::memory_order_acquire)) {
        // Preserve original bytes before a method changes the response mode.
        // If allocation fails we must still preserve this unknown call's ABI;
        // the known Finish method subsequently converts completion to failure.
        // The exact reviewed HTML branch never mixes these response modes.
        proxy->flush();
    }
    return original;
}
bool target_url(std::string_view url) noexcept {
    const auto end=url.find_first_of("?#");
    return url.substr(0,end)==route && url.find('\0')==url.npos && url.size()<2048;
}
bool insert_panel(std::string_view document,std::string_view payload,std::string& output) {
    output.clear();
    if(document.empty() || document.size()>max_document || payload.empty() || payload.size()>max_payload ||
       document.find('\0')!=document.npos || payload.find('\0')!=payload.npos || document.find(marker)!=document.npos) return false;
    const auto position=document.find("</body>");
    if(position==document.npos || document.find("</body>",position+7)!=document.npos) return false;
    output.reserve(document.size()+payload.size());
    output.append(document.substr(0,position));output.append(payload);output.append(document.substr(position));
    return true;
}
std::string start(const std::filesystem::path& root,ui::Service* bridge) {
    auto& s=state(); if(s.original) return "Native UI proof is already initialized";
    std::string payload="<!-- crml-native-ui-v1 -->";
    const auto append=[&](const wchar_t* name) {
        std::ifstream input(root/name,std::ios::binary|std::ios::ate);
        const auto length=input?input.tellg():std::streampos(-1);
        if(length<=0 || length>static_cast<std::streamoff>(max_payload-payload.size())) return false;
        std::string part(static_cast<size_t>(length),'\0');input.seekg(0);input.read(part.data(),static_cast<std::streamsize>(part.size()));
        if(!input || part.find('\0')!=part.npos) return false;
        payload+=part;return true;
    };
    if(bridge && !append(L"ui-bootstrap.html")) return "Native UI bridge unavailable: bootstrap missing or too large";
    if(std::filesystem::is_regular_file(root/"native-ui.enabled") && !append(L"native-ui-panel.html"))
        return "Native UI proof unavailable: panel payload missing or too large";
    if(bridge && payload.find("__CRML_UI_NONCE__")==payload.npos) return "Native UI bridge unavailable: bootstrap token missing";
    wchar_t executable[32768]{}; const auto exe_length=GetModuleFileNameW(nullptr,executable,32768);
    // This development proof relies on the complete reviewed callback control
    // flow. General unknown-build consent cannot substitute for that evidence.
    if(!exe_length || exe_length>=32768 || file_hash(executable)!=compatibility::tested_sha) return "Native UI proof unavailable: executable differs from the reviewed build";
    const auto module=GetModuleHandleW(L"cohtml.WindowsDesktop.dll"); if(!module) return "Native UI proof unavailable: engine UI library not loaded";
    wchar_t dll[32768]{}; const auto dll_length=GetModuleFileNameW(module,dll,32768);
    if(!dll_length || dll_length>=32768 || file_hash(dll)!=cohtml_sha) return "Native UI proof unavailable: engine UI library differs";
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto target=reinterpret_cast<void*>(image+0x34446b0);
    constexpr unsigned char signature[]{0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x40,0xfe,0xff,0xff};
    if(!compatibility::matches(target,signature,sizeof(signature)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+0x5110c38),&target,sizeof(target))) return "Native UI proof unavailable: resource handler differs";
    s.module_begin=reinterpret_cast<uintptr_t>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(s.module_begin+dos->e_lfanew);
    s.module_end=s.module_begin+nt->OptionalHeader.SizeOfImage;
    s.payload=std::move(payload);s.bridge=bridge;
    const auto initialized=MH_Initialize(); if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return "Native UI proof unavailable: hook initialization failed";
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&s.original))!=MH_OK) return "Native UI proof unavailable: resource hook conflict";
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);s.original=nullptr;return "Native UI proof unavailable: resource hook could not start";}
    if(s.bridge) s.bridge->enable(true);
    s.log.open(root/L"native-ui.jsonl",std::ios::trunc);
    if(s.log) s.log<<"{\"schema\":1,\"mode\":\"native-ui-resource-proof\",\"wasm_api\":"<<(bridge?"true":"false")<<"}\n";
    s.log.flush();
    s.enabled.store(true,std::memory_order_release);
    return bridge?"Bounded UI service armed; awaiting the engine UI document":"Native UI proof armed; the engine options page is extended when its document loads";
}
bool active() noexcept { return state().enabled.load(std::memory_order_acquire); }
void stop() noexcept {state().enabled.store(false,std::memory_order_release);if(state().bridge) state().bridge->enable(false);}
void poll() {
    auto& s=state(); const auto now=GetTickCount64(); if(now-s.last_report<1000) return;
    s.last_report=now;
    const std::array<uint64_t,11> counts{s.requests.load(),s.transformed.load(),s.passthrough.load(),s.failures.load(),s.exhausted.load(),s.completed.load(),s.pending.load(),s.observed.load(),s.foreign_response.load(),s.other_route.load(),s.source_mismatch.load()};
    if((counts==s.last_counts && !s.bridge) || !s.log) return;
    if(s.log.tellp()>512*1024) {s.log.close();return;}
    s.last_counts=counts;
    s.log<<"{\"requests\":"<<counts[0]<<",\"transformed\":"<<counts[1]<<",\"passthrough\":"<<counts[2]<<",\"failures\":"<<counts[3]<<",\"capacity_refusals\":"<<counts[4]<<",\"completed\":"<<counts[5]<<",\"pending\":"<<counts[6]<<",\"observed\":"<<counts[7]<<",\"foreign_response\":"<<counts[8]<<",\"other_route\":"<<counts[9]<<",\"source_mismatch\":"<<counts[10];
    if(s.bridge) {
        crml_ui_state snapshot{};
        const auto readable=s.bridge->ui_read(snapshot)>0;
        s.log<<",\"ui_polls\":"<<s.bridge->polls()<<",\"ui_submissions\":"<<s.bridge->submissions()<<",\"ui_acknowledgements\":"<<s.bridge->acknowledgements()
             <<",\"ui_fresh\":"<<(readable?"true":"false")<<",\"ui_screen\":"<<snapshot.screen<<",\"ui_actions\":"<<snapshot.actions<<",\"ui_generation\":"<<snapshot.generation;
    }
    s.log<<"}\n";
    s.log.flush();
}
}
