#include "lua_lifetime.h"
#include "lua_session.h"
#include "lua_references.h"
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ostream>

namespace crml::probe::lua::lifetime {
namespace {
using Close=void(*)(void*);
using Cleanup=void(*)(void*,void*,void*,void*,void*,void*,void*,uint64_t);
Close original_close{};
Cleanup original_cleanup{};
std::atomic<bool> enabled{},incomplete{};
std::atomic<uint64_t> read_failures{},missed{},cleanup_calls{},close_calls{};
SRWLOCK lock=SRWLOCK_INIT;
struct Identity {
    uintptr_t global{},world{};
    uint64_t vm_epoch{},world_epoch{},owner{},owner_epoch{},updates{};
};
struct Event {
    const char* kind{};
    uint64_t sequence{},tick{},vm{},world{},owner{},updates{};
    DWORD thread{};
};
std::array<Identity,16> identities{};
std::array<Event,256> events{};
size_t count{};
uint64_t serial{},sequence{};

// A missed teardown would make pointer reuse ambiguous. Stop assigning epochs
// after any loss, instead of treating a partial trace as lifetime proof.
bool enter() noexcept {
    if(!enabled.load(std::memory_order_acquire) || incomplete.load()) return false;
    if(!TryAcquireSRWLockExclusive(&lock)) {++missed;incomplete=true;return false;}
    if(!enabled.load() || incomplete.load()) {ReleaseSRWLockExclusive(&lock);return false;}
    return true;
}
template<class T> T read(uintptr_t p) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(p),sizeof(value));return value;
}
uintptr_t global_of(void* vm) noexcept {
    __try {return vm?read<uintptr_t>(reinterpret_cast<uintptr_t>(vm)+0x18):0;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        ++read_failures;incomplete=true;return 0;
    }
}
void* vm_of(void* state) noexcept {
    __try {return state?read<void*>(reinterpret_cast<uintptr_t>(state)+8):nullptr;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        ++read_failures;incomplete=true;return nullptr;
    }
}
Event event(const char* kind,const Identity& id) noexcept {
    return {kind,++sequence,GetTickCount64(),id.vm_epoch,id.world_epoch,id.owner_epoch,id.updates,GetCurrentThreadId()};
}
void append(Event value) noexcept {
    if(count==events.size()) {++missed;incomplete=true;return;}
    events[count++]=value;
}
Identity* find(uintptr_t global) noexcept {
    if(global) for(auto& id:identities) if(id.global==global) return &id;
    return nullptr;
}
// All post-call work uses copied diagnostic values. The original may free the
// entire state, its world or script owner; none is read again after forwarding.
void finish(Event token,const char* kind) noexcept {
    if(!token.kind || !enter()) return;
    token.kind=kind;token.sequence=++sequence;token.tick=GetTickCount64();token.thread=GetCurrentThreadId();
    append(token);ReleaseSRWLockExclusive(&lock);
}
void close_hook(void* vm) {
    Event token{};
    const bool tracked=session::configured();
    const bool recording=enabled.load(std::memory_order_acquire) && !incomplete.load();
    const auto global=(tracked || recording)?global_of(vm):0;
    if(tracked) session::close_begin(global);
    if(tracked) references::close(global);
    if(recording) {
        ++close_calls;
        if(enter()) {
            if(auto* id=find(global)) {
                token=event("vm_close_begin",*id);append(token);
                *id={}; // Retire before the engine allocator can reuse memory.
            }
            ReleaseSRWLockExclusive(&lock);
        }
    }
    original_close(vm);
    if(tracked) session::close_end();
    finish(token,"vm_close_end");
}
void cleanup_hook(void* state,void* b,void* c,void* d,void* e,void* f,void* g,uint64_t owner) {
    Event token{};
    const bool tracked=session::configured();
    const bool recording=enabled.load(std::memory_order_acquire) && !incomplete.load();
    const auto global=(tracked || recording)?global_of(vm_of(state)):0;
    if(tracked) session::cleanup_begin(global,owner);
    if(recording) {
        ++cleanup_calls;
        if(enter()) {
            if(auto* id=find(global);id && owner && id->owner==owner) {
                token=event("owner_cleanup_begin",*id);append(token);
                id->owner=0;id->owner_epoch=0;
            }
            ReleaseSRWLockExclusive(&lock);
        }
    }
    original_cleanup(state,b,c,d,e,f,g,owner);
    if(tracked) session::cleanup_end();
    finish(token,"owner_cleanup_end");
}
}

bool start(uintptr_t image) noexcept {
#if defined(CRML_LUA_PROBE)
    if(!image || original_close || original_cleanup) return false;
    constexpr unsigned char close_prefix[]{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x18};
    constexpr unsigned char cleanup_prefix[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20};
    auto* close_entry=reinterpret_cast<void*>(image+0x2c3be80);
    auto* cleanup_entry=reinterpret_cast<void*>(image+0x19c70e0);
    if(std::memcmp(close_entry,close_prefix,sizeof(close_prefix)) ||
       std::memcmp(cleanup_entry,cleanup_prefix,sizeof(cleanup_prefix))) return false;
    // Installed trampolines stay pinned, including partial installation failures.
    // No state is observed until BOTH hooks are active; stopped hooks forward only.
    if(MH_CreateHook(close_entry,reinterpret_cast<void*>(&close_hook),reinterpret_cast<void**>(&original_close))!=MH_OK ||
       MH_CreateHook(cleanup_entry,reinterpret_cast<void*>(&cleanup_hook),reinterpret_cast<void**>(&original_cleanup))!=MH_OK ||
       MH_EnableHook(close_entry)!=MH_OK || MH_EnableHook(cleanup_entry)!=MH_OK) return false;
    enabled.store(true,std::memory_order_release);return true;
#else
    (void)image;return false;
#endif
}
void observe(void* vm,uintptr_t world,uint64_t owner) noexcept {
    if(!enabled.load(std::memory_order_acquire) || incomplete.load() || !world || !owner) return;
    const auto global=global_of(vm);
    if(!global || !enter()) return;
    auto* id=find(global);
    if(!id) {
        for(auto& slot:identities) if(!slot.global) {id=&slot;break;}
        if(!id) {++missed;incomplete=true;ReleaseSRWLockExclusive(&lock);return;}
        id->global=global;id->vm_epoch=++serial;append(event("vm_observed",*id));
    }
    if(id->world!=world) {
        if(id->world) append(event("world_changed",*id));
        id->world=world;id->world_epoch=++serial;id->owner=0;id->owner_epoch=0;
        append(event("world_observed",*id));
    }
    if(!id->owner) {
        id->owner=owner;id->owner_epoch=++serial;append(event("owner_observed",*id));
    }
    ++id->updates;
    // Record the first two and sparse subsequent update observations, allowing
    // cross-update thread comparisons without logging every engine script call.
    if(id->updates<=2 || id->updates%4096==0) append(event("update",*id));
    ReleaseSRWLockExclusive(&lock);
}
bool active() noexcept {return enabled.load(std::memory_order_acquire) && !incomplete.load();}
void write(std::ostream& out) {
    std::array<Event,256> batch{};size_t size{};
    AcquireSRWLockExclusive(&lock);
    size=count;std::copy_n(events.begin(),size,batch.begin());count=0;
    ReleaseSRWLockExclusive(&lock);
    out<<"{\"type\":\"lua_lifetime\",\"schema\":1,\"enabled\":"<<(enabled.load()?"true":"false")
       <<",\"incomplete\":"<<(incomplete.load()?"true":"false")<<",\"missed\":"<<missed.load()
       <<",\"read_failures\":"<<read_failures.load()<<",\"cleanup_calls\":"<<cleanup_calls.load()
       <<",\"close_calls\":"<<close_calls.load()<<"}\n";
    for(size_t i=0;i<size;++i) {
        const auto& v=batch[i];
        out<<"{\"type\":\"lua_lifetime_event\",\"schema\":1,\"event\":\""<<v.kind<<"\",\"sequence\":"<<v.sequence
           <<",\"tick_ms\":"<<v.tick<<",\"thread\":"<<v.thread<<",\"vm_epoch\":"<<v.vm
           <<",\"world_epoch\":"<<v.world<<",\"owner_epoch\":"<<v.owner<<",\"updates\":"<<v.updates<<"}\n";
    }
}
void stop() noexcept {
    AcquireSRWLockExclusive(&lock);
    if(enabled.exchange(false)) append(event("observer_stop",{}));
    ReleaseSRWLockExclusive(&lock);
}
#ifdef CRML_LUA_LIFETIME_TESTING
namespace testing {
void configure(Close close,Cleanup cleanup) {
    stop();AcquireSRWLockExclusive(&lock);
    identities={};events={};count=0;serial=sequence=0;
    original_close=close;original_cleanup=cleanup;
    incomplete=false;read_failures=0;missed=0;cleanup_calls=0;close_calls=0;
    enabled=true;ReleaseSRWLockExclusive(&lock);
}
void close(void* vm) {close_hook(vm);}
void cleanup(void* state,void* b,void* c,void* d,void* e,void* f,void* g,uint64_t owner) {
    cleanup_hook(state,b,c,d,e,f,g,owner);
}
}
#endif
}
