#include "native_ui.h"
#include "../compatibility.h"
#include "../ui_service.h"
#include "../settings_ui.h"
#include "../mod_feedback.h"
#include "../mod_drawing.h"
#include "../mod_lists.h"
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
#include <intrin.h>

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
    bool settings{};
    bool feedback{};
    bool tutorials{};
    bool drawing{};
    bool lists{};
    std::string_view expected_html_sha{html_sha};
    Handler original{};
    uintptr_t module_begin{},module_end{};
    std::ofstream log;
    uint64_t last_report{};
    std::array<uint64_t,11> last_counts{};
};
State& state() { static auto* value=new State; return *value; }

// The map's ECS input/position dispatchers consume engine actions separately
// from Cohtml text events. Suspend only these dispatchers for a short, renewed
// editor lease; never filter Windows keyboard messages used for typing.
using MapDispatch=void(*)(void*,void*,void*);
MapDispatch map_input_original{},map_position_original{},gameplay_menu_input_original{};
bool map_editor_hooks{};
using MarkerAdd=bool(*)(const void*,const void*,uint32_t,void*);
MarkerAdd marker_add_original{};
uintptr_t marker_add_caller{};
using MarkerRemove=void(*)(uint32_t,void*);
MarkerRemove marker_remove_original{};
uintptr_t marker_remove_caller{};
bool marker_actions_hooked{};
bool game_foreground() noexcept {
    DWORD process{};const auto window=GetForegroundWindow();
    return window&&GetWindowThreadProcessId(window,&process)&&process==GetCurrentProcessId();
}
bool consume_hovered_annotation() noexcept {
    const auto window=GetForegroundWindow();DWORD process{};POINT cursor{};RECT client{};
    if(!window||!GetWindowThreadProcessId(window,&process)||process!=GetCurrentProcessId()||
       !GetCursorPos(&cursor)||!ScreenToClient(window,&cursor)||!GetClientRect(window,&client))return false;
    const double width=static_cast<double>(client.right)-client.left;
    const double height=static_cast<double>(client.bottom)-client.top;
    if(width<=0||height<=0||cursor.x<client.left||cursor.x>=client.right||
       cursor.y<client.top||cursor.y>=client.bottom)return false;
    const std::array<float,2> normalized{
        static_cast<float>((static_cast<double>(cursor.x)-client.left)/width),
        static_cast<float>((static_cast<double>(cursor.y)-client.top)/height)};
    return process_drawing().hovered_annotation_action(normalized);
}
bool marker_arguments(const void* point,void* database,std::array<float,2>& copied,bool& full) noexcept {
    __try {
        std::memcpy(copied.data(),point,sizeof(copied));
        const auto base=reinterpret_cast<uintptr_t>(database);
        const auto count=*reinterpret_cast<const uint32_t*>(base+0x50);
        const auto records=*reinterpret_cast<const uintptr_t*>(base+0x48);
        if(count>6||(!records&&count))return false;
        unsigned occupied=0;
        for(unsigned i=0;i<count;++i){const auto id=*reinterpret_cast<const uint32_t*>(records+i*48);if(id>=1&&id<=6)occupied|=1u<<(id-1);}
        full=occupied==63;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool marker_add(const void* world,const void* point,uint32_t district,void* database) {
    std::array<float,2> copied{};bool full{};
    if(reinterpret_cast<uintptr_t>(_ReturnAddress())==marker_add_caller&&game_foreground()) {
        if(consume_hovered_annotation())return true;
        if(marker_arguments(point,database,copied,full)&&process_drawing().placement_action(copied,full))return true;
    }
    return marker_add_original(world,point,district,database);
}
void marker_remove(uint32_t slot,void* database) {
    if(reinterpret_cast<uintptr_t>(_ReturnAddress())==marker_remove_caller&&game_foreground()&&
       consume_hovered_annotation())return;
    marker_remove_original(slot,database);
}
bool start_marker_placement(uintptr_t image) {
    if(marker_actions_hooked)return true;
    if(!compatibility::reviewed_build||compatibility::engine_profile!=compatibility::EngineProfile::october_patch)return false;
    auto* add=reinterpret_cast<void*>(image+0x1f50fa0);
    auto* remove=reinterpret_cast<void*>(image+0x1f510d0);
    constexpr unsigned char add_bytes[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x50};
    constexpr unsigned char add_call[]{0xe8,0xe9,0x32,0x01,0x00};
    constexpr unsigned char remove_bytes[]{0x40,0x53,0x48,0x83,0xec,0x20,0x8b,0x42,0x50,0x48,0x8b,0xda,0x45,0x33,0xc0};
    constexpr unsigned char remove_call[]{0xe8,0x71,0x34,0x01,0x00};
    if(!compatibility::matches(add,add_bytes,sizeof(add_bytes))||
       !compatibility::matches(reinterpret_cast<void*>(image+0x1f3dcb2),add_call,sizeof(add_call))||
       !compatibility::matches(remove,remove_bytes,sizeof(remove_bytes))||
       !compatibility::matches(reinterpret_cast<void*>(image+0x1f3dc5a),remove_call,sizeof(remove_call)))return false;
    marker_add_caller=image+0x1f3dcb7;
    marker_remove_caller=image+0x1f3dc5f;
    if(MH_CreateHook(add,reinterpret_cast<void*>(&marker_add),reinterpret_cast<void**>(&marker_add_original))!=MH_OK)return false;
    if(MH_CreateHook(remove,reinterpret_cast<void*>(&marker_remove),reinterpret_cast<void**>(&marker_remove_original))!=MH_OK){
        MH_RemoveHook(add);marker_add_original=nullptr;return false;
    }
    if(MH_EnableHook(add)!=MH_OK){
        MH_RemoveHook(remove);MH_RemoveHook(add);marker_add_original=nullptr;marker_remove_original=nullptr;return false;
    }
    if(MH_EnableHook(remove)!=MH_OK){
        MH_DisableHook(add);MH_RemoveHook(remove);MH_RemoveHook(add);
        marker_add_original=nullptr;marker_remove_original=nullptr;return false;
    }
    marker_actions_hooked=true;return true;
}
std::atomic<uint64_t> map_inputs_suspended{},map_positions_suspended{},gameplay_menu_inputs_suspended{};
bool map_editing() noexcept {
    return game_foreground()&&process_drawing().map_editor_input_active();
}
void map_input_dispatch(void* a,void* b,void* c){if(map_editing())++map_inputs_suspended;else map_input_original(a,b,c);}
void map_position_dispatch(void* a,void* b,void* c){if(map_editing())++map_positions_suspended;else map_position_original(a,b,c);}
void gameplay_menu_input_dispatch(void* a,void* b,void* c){if(map_editing())++gameplay_menu_inputs_suspended;else gameplay_menu_input_original(a,b,c);}
bool start_map_editor_input(uintptr_t image) {
    if(map_editor_hooks)return true;
    if(!compatibility::reviewed_build||compatibility::engine_profile!=compatibility::EngineProfile::october_patch)return false;
    auto* input=reinterpret_cast<void*>(image+0x1f48bd0);
    auto* position=reinterpret_cast<void*>(image+0x1f3a250);
    auto* menu=reinterpret_cast<void*>(image+0x1fbe970);
    constexpr unsigned char input_bytes[]{0x4c,0x8b,0xdc,0x55,0x53,0x56,0x41,0x55,0x49,0x8d,0xab,0x48,0xff,0xff,0xff,0x48,0x81,0xec,0x98,0x01,0,0};
    constexpr unsigned char position_bytes[]{0x49,0x8b,0x40,0x58,0x4c,0x8b,0x48,0x18,0x4c,0x8b,0x40,0x10,0x48,0x8b,0x50,0x08,0x48,0x8b,0x08,0xe9,0x48,0xe7,0xff,0xff};
    constexpr unsigned char menu_bytes[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x60};
    if(!compatibility::matches(input,input_bytes,sizeof(input_bytes))||!compatibility::matches(position,position_bytes,sizeof(position_bytes))||!compatibility::matches(menu,menu_bytes,sizeof(menu_bytes)))return false;
    // Acquire the complete input scope before acknowledging an editable field.
    // A partial installation must not leave map or parent-menu input suspended.
    struct Route {void* target;void* hook;MapDispatch* original;};
    const std::array<Route,3> routes{{{input,reinterpret_cast<void*>(&map_input_dispatch),&map_input_original},
        {position,reinterpret_cast<void*>(&map_position_dispatch),&map_position_original},
        {menu,reinterpret_cast<void*>(&gameplay_menu_input_dispatch),&gameplay_menu_input_original}}};
    size_t created=0,enabled=0;
    for(const auto& route:routes){if(MH_CreateHook(route.target,route.hook,reinterpret_cast<void**>(route.original))!=MH_OK)break;++created;}
    if(created==routes.size())for(const auto& route:routes){if(MH_EnableHook(route.target)!=MH_OK)break;++enabled;}
    if(enabled!=routes.size()){
        while(enabled)MH_DisableHook(routes[--enabled].target);
        while(created)MH_RemoveHook(routes[--created].target);
        return false;
    }
    map_editor_hooks=true;return true;
}

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
                    if(proxy->owner->settings && !payload.empty()) {
                        constexpr std::string_view token="__CRML_SETTINGS_PAGE__";
                        const auto position=payload.find(token);
                        const auto page=position!=payload.npos?process_settings_ui().open_page():0;
                        if(page)payload.replace(position,token.size(),std::to_string(page));
                        else payload.clear();
                    }
                    if(proxy->owner->drawing && !payload.empty()) {
                        constexpr std::string_view token="__CRML_DRAWING_PAGE__";
                        const auto position=payload.find(token);
                        const auto page=position!=payload.npos?process_drawing().open_page():0;
                        if(page)payload.replace(position,token.size(),std::to_string(page));
                        else payload.clear();
                    }
                    if(proxy->owner->feedback && !payload.empty()) {
                        constexpr std::string_view token="__CRML_FEEDBACK_PAGE__";
                        const auto position=payload.find(token);
                        const auto page=position!=payload.npos?process_feedback().open_page():0;
                        if(page)payload.replace(position,token.size(),std::to_string(page));
                        else payload.clear();
                    }
                    std::string tutorial_document;
                    const auto document=proxy->owner->tutorials && bind_tutorial_layout(source,tutorial_document)
                        ?std::string_view(tutorial_document):std::string_view(source);
                    changed=insert_panel(document,payload,rewritten);
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
    if(!url.starts_with(ui::poll_prefix) && !url.starts_with(ui::result_poll_prefix)) return false;
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
bool serve_author(std::string_view url,void* response,bool feedback,bool drawing=false) {
    if(!url.starts_with(drawing?drawing_prefix:feedback?feedback_prefix:settings_prefix))return false;
    std::string body;const auto code=drawing?process_drawing().exchange(url,body):feedback?process_feedback().exchange(url,body):process_settings_ui().exchange(url,body);
    auto** table=*static_cast<void***>(response);
    const auto allocate=reinterpret_cast<GetBuffer>(table[1]);
    const auto status=reinterpret_cast<Status>(table[5]);
    const auto complete=reinterpret_cast<Finish>(table[8]);
    status(response,static_cast<uint32_t>(code));
    auto* buffer=body.empty()?nullptr:allocate(response,body.size());
    if(buffer)std::memcpy(buffer,body.data(),body.size());
    complete(response,(!body.empty() && !buffer)?1u:0u);return true;
}
bool serve_settings(std::string_view url,void* response) {return serve_author(url,response,false);}
void hook(void* handler,void* request,void* response) {
    auto& s=state(); char url[2048]{};
    bool selected=false;
    if(s.enabled.load(std::memory_order_acquire)) {
        ++s.observed;
        if(!request || !response || !readable_request(request,response,url,s)) ++s.foreign_response;
        else if(s.settings && serve_settings(url,response)) return;
        else if(s.feedback && serve_author(url,response,true)) return;
        else if(s.drawing && serve_author(url,response,false,true)) return;
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
bool bind_tutorial_layout(std::string_view document,std::string& output) {
    output.clear();
    if(document.empty() || document.size()>max_document || document.find('\0')!=document.npos ||
       document.find("data-bind-crml-tutorial-layout")!=document.npos) return false;
    constexpr std::string_view models[]{"ui_tutorial_dynamic_message_text.translation","ui_tutorial_static_message_text.translation"};
    for(const auto model:models) {
        const auto attribute=std::string("data-bind-html=\"{{")+std::string(model)+"}}\"";
        const auto at=document.find(attribute);
        if(at==document.npos || document.find(attribute,at+attribute.size())!=document.npos) return false;
    }
    output=document;
    for(const auto model:models) {
        const auto attribute=std::string("data-bind-html=\"{{")+std::string(model)+"}}\"";
        output.insert(output.find(attribute)+attribute.size()," data-bind-crml-tutorial-layout=\"{{"+std::string(model)+"}}\"");
    }
    return true;
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
struct LoadedPayload {
    std::string text="<!-- crml-native-ui-v1 -->",warnings;
    bool bridge{},settings{},feedback{},tutorials{},drawing{};
};
LoadedPayload load_payload(const std::filesystem::path& root,bool bridge,bool settings,bool feedback,bool tutorials=false,bool drawing=false) {
    LoadedPayload result;
    const auto append=[&](const wchar_t* name,std::string_view token,bool requested,std::string_view label) {
        if(!requested)return false;
        std::ifstream input(root/name,std::ios::binary|std::ios::ate);
        const auto length=input?input.tellg():std::streampos(-1);
        if(length>0 && length<=64*1024 && length<=static_cast<std::streamoff>(max_payload-result.text.size())) {
            std::string part(static_cast<size_t>(length),'\0');input.seekg(0);input.read(part.data(),static_cast<std::streamsize>(part.size()));
            const auto position=part.find(token);
            if(input && part.find('\0')==part.npos && position!=part.npos && part.find(token,position+token.size())==part.npos) {
                result.text+=part;return true;
            }
        }
        result.warnings+=std::string(label)+" UI unavailable: missing or invalid payload. ";return false;
    };
    result.bridge=append(L"ui-bootstrap.html","__CRML_UI_NONCE__",bridge,"Startup");
    result.settings=append(L"native-ui-panel.html","__CRML_SETTINGS_PAGE__",settings,"Settings");
    result.feedback=append(L"native-ui-feedback.html","__CRML_FEEDBACK_PAGE__",feedback,"Feedback");
    result.tutorials=append(L"native-ui-tutorials.html","crml-native-tutorial-layout-v1",tutorials,"Tutorial image");
    result.drawing=append(L"native-ui-drawing.html","__CRML_DRAWING_PAGE__",drawing,"Drawing");
    return result;
}
std::string start(const std::filesystem::path& root,ui::Service* bridge,bool settings,bool feedback,bool tutorials,bool drawing,bool lists) {
    auto& s=state(); if(s.original) return "Native UI proof is already initialized";
    settings=settings || lists || std::filesystem::is_regular_file(root/"native-ui.enabled");
    auto loaded=load_payload(root,bridge!=nullptr,settings,feedback,tutorials,drawing);
    bridge=loaded.bridge?bridge:nullptr;settings=loaded.settings;feedback=loaded.feedback;tutorials=loaded.tutorials;drawing=loaded.drawing;lists=lists&&settings;
    if(!bridge && !settings && !feedback && !tutorials && !drawing)return loaded.warnings.empty()?"No native UI services requested":loaded.warnings;
    wchar_t executable[32768]{}; const auto exe_length=GetModuleFileNameW(nullptr,executable,32768);
    // This development proof relies on the complete reviewed callback control
    // flow. General unknown-build consent cannot substitute for that evidence.
    if(!exe_length || exe_length>=32768 || !compatibility::known_build(file_hash(executable))) return "Native UI proof unavailable: executable differs from the reviewed builds";
    const auto module=GetModuleHandleW(L"cohtml.WindowsDesktop.dll"); if(!module) return "Native UI proof unavailable: engine UI library not loaded";
    wchar_t dll[32768]{}; const auto dll_length=GetModuleFileNameW(module,dll,32768);
    if(!dll_length || dll_length>=32768 || file_hash(dll)!=cohtml_sha) return "Native UI proof unavailable: engine UI library differs";
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto target=reinterpret_cast<void*>(compatibility::address(image,0x34446b0));
    constexpr unsigned char signature[]{0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x40,0xfe,0xff,0xff};
    if(!compatibility::matches_code(target,0x34446b0,signature,sizeof(signature)) ||
       !compatibility::matches(reinterpret_cast<void*>(compatibility::address(image,0x5110c38)),&target,sizeof(target))) return "Native UI proof unavailable: resource handler differs";
    s.module_begin=reinterpret_cast<uintptr_t>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(s.module_begin+dos->e_lfanew);
    s.module_end=s.module_begin+nt->OptionalHeader.SizeOfImage;
    s.expected_html_sha=compatibility::ui_document_sha();
    s.payload=std::move(loaded.text);s.bridge=bridge;s.settings=settings;s.feedback=feedback;s.tutorials=tutorials;s.drawing=drawing;s.lists=lists;
    const auto initialized=MH_Initialize(); if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return "Native UI proof unavailable: hook initialization failed";
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&s.original))!=MH_OK) return "Native UI proof unavailable: resource hook conflict";
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);s.original=nullptr;return "Native UI proof unavailable: resource hook could not start";}
    if(s.bridge) s.bridge->enable(true);
    if(s.settings)process_settings_ui().enable(true);
    if(s.feedback)process_feedback().enable_renderer(true);
    if(s.drawing){process_drawing().enable_renderer(true);process_drawing().enable_map_editor_input(start_map_editor_input(image));start_marker_placement(image);}
    if(s.lists)process_lists().enable_renderer(true);
    s.log.open(root/L"native-ui.jsonl",std::ios::trunc);
    if(s.log) s.log<<"{\"schema\":1,\"mode\":\"native-ui-resource-proof\",\"wasm_api\":"<<(bridge?"true":"false")<<"}\n";
    s.log.flush();
    s.enabled.store(true,std::memory_order_release);
    return loaded.warnings+"Native UI services armed; awaiting the engine UI document";
}
bool active() noexcept { return state().enabled.load(std::memory_order_acquire); }
void stop() noexcept {state().enabled.store(false,std::memory_order_release);if(state().bridge) state().bridge->enable(false);process_settings_ui().enable(false);process_feedback().enable_renderer(false);process_drawing().enable_renderer(false);process_lists().enable_renderer(false);}
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
    if(s.drawing)s.log<<",\"drawing\":"<<process_drawing().diagnostics()<<",\"map_editor_hooks\":"<<(map_editor_hooks?"true":"false")
        <<",\"marker_placement_hooked\":"<<(marker_actions_hooked?"true":"false")
        <<",\"map_inputs_suspended\":"<<map_inputs_suspended.load()<<",\"map_positions_suspended\":"<<map_positions_suspended.load()
        <<",\"gameplay_menu_inputs_suspended\":"<<gameplay_menu_inputs_suspended.load();
    s.log<<"}\n";
    s.log.flush();
}
}
