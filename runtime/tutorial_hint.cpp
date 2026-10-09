#include "tutorial_hint.h"
#include "tutorial_payload.h"
#include "mod_tutorials.h"
#include "compatibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>

namespace crml::tutorial {
namespace {
using Worker=void(*)(const void*,void*,const void*,const void*,void*);
using InputWorker=void(*)(const void*,const void*,const void*);
constexpr uintptr_t worker_rva=0x1fe9040,caller_rva=0x1fea159;
constexpr uintptr_t input_rva=0x1fe8e60,input_caller_rva=0x1fea404;
Worker original{};
InputWorker original_input{};
PayloadNative payload_native{};
ModTutorials* service{};
uintptr_t image{};
std::atomic<bool> accepting{};
std::atomic<bool> prompt_accepting{};
std::mutex mutex;
bool installed{},input_installed{},poisoned{};

template<class T> T read(const void* p,size_t offset=0) noexcept {
    T out{};std::memcpy(&out,static_cast<const unsigned char*>(p)+offset,sizeof(out));return out;
}
template<class T,size_t N> void put(std::array<unsigned char,N>& b,size_t offset,T value) noexcept {
    std::memcpy(b.data()+offset,&value,sizeof(value));
}
bool accessible(uintptr_t start,size_t size) noexcept {
    if(!start || !size || start>std::numeric_limits<uintptr_t>::max()-size) return false;
    const auto end=start+size;
    while(start<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(start),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto p=info.Protect&0xff;
        if(p!=PAGE_READONLY && p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READ &&
           p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY) return false;
        const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(base>std::numeric_limits<uintptr_t>::max()-info.RegionSize || base+info.RegionSize<=start) return false;
        start=base+info.RegionSize;
    }
    return true;
}
// Read-only private native table. Only the sync worker receives this query;
// no private identifier enters game ECS, request, input, completion or save data.
// Lookup 0x1e63e00 uses the native 0x68 slot and 0xde5fb9d2630458e9 hash.
struct Session {
    ModTutorials::Request request{};
    PageVector pages{};
    std::array<unsigned char,0x70> data{};
    std::array<unsigned char,48> controls{};
    std::array<unsigned char,32*0x68> slots{};
    std::array<unsigned char,0x38> state{};
    uint64_t started{};
    bool presented{},retiring{},cancelled{},suspended{};
    void prepare() noexcept {
        data.fill(0);controls.fill(0x80);slots.fill(0);state.fill(0);
        constexpr uint64_t hash=0xde5fb9d2630458e9ULL;
        constexpr size_t slot=(hash>>7)&31;
        static_assert(slot<31);
        controls[31]=0xff;controls[slot]=static_cast<unsigned char>(hash&0x7f);
        if constexpr(slot<16) controls[slot+32]=controls[slot];
        put(data,0,uint32_t{1});data[4]=1;
        put(data,8,reinterpret_cast<uintptr_t>(controls.data()));
        put(data,0x10,reinterpret_cast<uintptr_t>(slots.data()));
        put(data,0x18,uint64_t{1});put(data,0x20,uint64_t{31});put(data,0x30,uint64_t{27});
        put(slots,slot*0x68,uint32_t{1});
        std::memcpy(slots.data()+slot*0x68+8,&pages,sizeof(pages));
        // Lookup returns the value at slot+8. Value +0x18 selects the stock
        // CLOSE_TUTORIAL callout/input mode;
        // +0x1c remains the stock dynamic kind. The prompt's input worker
        // receives only a private Requests vector, never game progression.
        if(request.kind==CRML_TUTORIAL_PROMPT &&
           (request.options&CRML_TUTORIAL_OPTION_NATIVE_DISMISS))
            put(slots,slot*0x68+8+0x18,uint32_t{1});
        started=0;presented=false;retiring=false;cancelled=false;suspended=false;
    }
};
Session session;
bool game_busy(const void* query,const void* state,bool& busy) noexcept {
    __try {
        if(!accessible(reinterpret_cast<uintptr_t>(query),24) || !accessible(reinterpret_cast<uintptr_t>(state),0x38)) return false;
        const auto base=read<uintptr_t>(query),row=read<uint64_t>(query,16);
        if(row>=16384 || base>std::numeric_limits<uintptr_t>::max()-row*0x70) return false;
        const auto data=base+row*0x70;
        if(!accessible(data,0x70)) return false;
        busy=read<uint8_t>(reinterpret_cast<void*>(data),4)!=0 || read<uint32_t>(state,4)!=0;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
void fail() noexcept {
    poisoned=true;accepting=false;
    prompt_accepting=false;
    service->enable(CRML_TUTORIAL_HINT,false);
    service->enable(CRML_TUTORIAL_PROMPT,false);
    if(session.request.ticket) service->report(session.request.ticket,CRML_TUTORIAL_FAILED);
    // A fault can interrupt construction/destruction. Retain at most this one
    // allocation and permanently disable admission; never guess its ownership.
}
void finish() noexcept {
    if(!payload_native.destroy(session.pages)) {fail();return;}
    service->report(session.request.ticket,session.cancelled?CRML_TUTORIAL_CANCELLED:CRML_TUTORIAL_DISMISSED);
    session.request={};session.state.fill(0);session.data.fill(0);
}
bool draw(const void* c,const void* d,void* facts) noexcept {
    const std::array<uintptr_t,3> query{reinterpret_cast<uintptr_t>(session.data.data()),0,0};
    __try {original(query.data(),session.state.data(),c,d,facts);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool input_game_idle(const void* query) noexcept {
    __try {
        if(!accessible(reinterpret_cast<uintptr_t>(query),32)) return false;
        const auto base=read<uintptr_t>(query),row=read<uint64_t>(query,24);
        if(row>=16384 || base>std::numeric_limits<uintptr_t>::max()-row*0x70) return false;
        const auto data=base+row*0x70;
        return accessible(data,0x70) && read<uint8_t>(reinterpret_cast<void*>(data),4)==0;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
// Only this borrowed native input callback may use the engine's live Input
// backing. Capacity one prevents the native vector path from allocating.
// Its completion event stays in this stack-local vector and is consumed here.
int private_input(const void* input) noexcept {
    const uint32_t id=read<uint32_t>(session.state.data());
    uint64_t item{};
    std::array<unsigned char,16> requests{};
    put(requests,0,reinterpret_cast<uintptr_t>(&item));
    put(requests,8,uint32_t{0});put(requests,12,uint32_t{1});
    const std::array<uintptr_t,4> query{reinterpret_cast<uintptr_t>(session.data.data()),
        reinterpret_cast<uintptr_t>(requests.data()),0,0};
    bool fired{};
    __try {
        original_input(query.data(),&id,input);
        if(read<uint32_t>(requests.data(),8)>1) return -1;
        fired=read<uint32_t>(requests.data(),8)==1 && static_cast<uint8_t>(item)==3 &&
            static_cast<uint32_t>(item>>32)==id;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
    return fired?1:0;
}
void update_input(const void* query,const void* input) {
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock() || !prompt_accepting || poisoned || !service ||
       session.request.kind!=CRML_TUTORIAL_PROMPT || !session.presented ||
       session.retiring || session.suspended ||
       !(session.request.options&CRML_TUTORIAL_OPTION_NATIVE_DISMISS) ||
       !read<uint32_t>(session.state.data()) || !input_game_idle(query)) return;
    const int result=private_input(input);
    if(result<0) fail();
    else if(result>0) session.retiring=true;
}
void input_hook(const void* a,const void* b,const void* c) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    original_input(a,b,c);
    if(caller!=image+input_caller_rva) return;
    try {update_input(a,c);}
    catch(...) {std::lock_guard lock(mutex);if(service) fail();}
}
void update(const void* query,void* state,const void* c,const void* d,void* facts,uint64_t now) {
    // A native model callback can reenter engine work; never block that work
    // waiting on the private hint currently publishing the callback.
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()) return;
    if(poisoned || !service) return;
    bool busy{};
    if(!game_busy(query,state,busy)) return;
    if(!session.request.ticket) {
        if(busy || !accepting || !service->take_dynamic(session.request)) return;
        if(service->cancelled(session.request.ticket)) {
            service->report(session.request.ticket,CRML_TUTORIAL_CANCELLED);session.request={};return;
        }
        // Do not inject format characters: the native font renders them as boxes.
        const auto body=tutorial_body_markup(session.request.body,session.request.image_url,session.request.image);
        if(!payload_native.make(session.request.title,body,session.pages)) {fail();return;}
        session.prepare();
    }
    session.cancelled=session.cancelled || !accepting || service->cancelled(session.request.ticket);
    session.retiring=session.retiring || session.cancelled ||
        (session.presented && now-session.started>=session.request.duration_ms);
    if(busy) {
        // The real sync already replaced/hid our model. Its own state and
        // selection are untouched. Private data has no asynchronous reader.
        session.state.fill(0);session.suspended=true;
        if(session.retiring) finish();
        return;
    }
    if(session.suspended) {session.state.fill(0);session.suspended=false;}
    session.data[4]=session.retiring?0:1;
    if(session.request.kind==CRML_TUTORIAL_PROMPT) {
        constexpr uint64_t hash=0xde5fb9d2630458e9ULL;
        constexpr size_t slot=(hash>>7)&31;
        auto* value=session.slots.data()+slot*0x68+8;
        const float duration=(session.request.options&CRML_TUTORIAL_OPTION_PROGRESS)?
            session.request.duration_ms/1000.0f:0.0f;
        const float elapsed=session.presented?static_cast<float>(std::min<uint64_t>(now-session.started,
            session.request.duration_ms))/1000.0f:0.0f;
        std::memcpy(value+0x48,&duration,sizeof(duration));
        std::memcpy(value+0x4c,&elapsed,sizeof(elapsed));
    }
    if(!draw(c,d,facts)) {fail();return;}
    if(!session.presented && read<uint32_t>(session.state.data(),4)!=0) {
        session.presented=true;session.started=now;
        service->report(session.request.ticket,CRML_TUTORIAL_PRESENTED);
    }
    // State 0 alone is insufficient: one later native sync must also clear
    // displayed ID, which otherwise survives the native fade's final frame.
    if(session.retiring && !read<uint32_t>(session.state.data(),4) && !read<uint32_t>(session.state.data())) finish();
}
void hook(const void* a,void* b,const void* c,const void* d,void* e) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    original(a,b,c,d,e);
    if(caller!=image+caller_rva) return;
    try {update(a,b,c,d,e,GetTickCount64());}
    catch(...) {std::lock_guard lock(mutex);if(service) fail();}
}
}
bool initialize_hint(uintptr_t base) noexcept {
    std::lock_guard lock(mutex);
    if(installed) return accepting;
    if(poisoned || !PayloadNative::bind(base,payload_native)) return false;
    constexpr unsigned char prefix[]{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x55,0x53,0x56,0x57,0x41,
        0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa9,0x48,0x81,0xec,0x98,0x00};
    auto* target=reinterpret_cast<void*>(base+worker_rva);
    if(!compatibility::matches(target,prefix,sizeof(prefix))) return false;
    auto status=MH_Initialize();if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(hook),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    image=base;service=&process_tutorials();accepting=true;
    if(MH_EnableHook(target)!=MH_OK) {accepting=false;MH_RemoveHook(target);original=nullptr;return false;}
    installed=true;service->enable(CRML_TUTORIAL_HINT,true);
    // Failure here leaves the original passive hint service available.
    constexpr unsigned char input_prefix[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,
        0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x40,0x49,0x8b,0xf0};
    constexpr unsigned char input_call[]{0xe8,0x5c,0xea,0xff,0xff};
    auto* input_target=reinterpret_cast<void*>(base+input_rva);
    if(compatibility::matches(input_target,input_prefix,sizeof(input_prefix)) &&
       compatibility::matches(reinterpret_cast<void*>(base+input_caller_rva-sizeof(input_call)),
           input_call,sizeof(input_call)) &&
       MH_CreateHook(input_target,reinterpret_cast<void*>(input_hook),reinterpret_cast<void**>(&original_input))==MH_OK) {
        if(MH_EnableHook(input_target)==MH_OK) {
            input_installed=true;prompt_accepting=true;service->enable(CRML_TUTORIAL_PROMPT,true);
        } else {MH_RemoveHook(input_target);original_input=nullptr;}
    }
    return true;
}
bool hint_available() noexcept {return accepting;}
void shutdown_hint() noexcept {
    accepting=false;
    prompt_accepting=false;
    process_tutorials().enable(CRML_TUTORIAL_HINT,false);
    process_tutorials().enable(CRML_TUTORIAL_PROMPT,false);
}
#ifdef CRML_TUTORIAL_HINT_TESTING
void hint_test_initialize(HintWorker worker,const PayloadNative& payload,ModTutorials& test_service,HintInputWorker input) {
    std::lock_guard lock(mutex);
    session=Session{};original=worker;original_input=input;payload_native=payload;service=&test_service;
    poisoned=false;accepting=true;prompt_accepting=input!=nullptr;
    service->enable(CRML_TUTORIAL_HINT,true);
    service->enable(CRML_TUTORIAL_PROMPT,input!=nullptr);
}
void hint_test_sync(const void* a,void* b,void* facts,uint64_t now) {
    original(a,b,nullptr,nullptr,facts);update(a,b,nullptr,nullptr,facts,now);
}
void hint_test_input(const void* a,const void* b,const void* c) {
    original_input(a,b,c);update_input(a,c);
}
bool hint_test_has_payload() {std::lock_guard lock(mutex);return session.pages.data!=nullptr;}
#endif
}
