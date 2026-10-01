#include "compatibility.h"
#include "lua_references.h"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <ostream>

namespace crml::probe::lua::references {
namespace {
using Retain=int(*)(void*,int);
using Release=void(*)(void*,int);
Retain original_retain{};
Release original_release{};
uintptr_t image_base{};
std::atomic<bool> enabled{};
std::atomic<uint64_t> read_failures{};
SRWLOCK gate=SRWLOCK_INIT;
struct Ref {uintptr_t global{};int reference{};};
struct Report {
    std::array<Ref,16> refs{};
    uint64_t retained{},released{},explicit_removals{},owner_removals{},error_removals{},other_removals{},vm_reclaimed{},pending{},lost{};
    DWORD retain_thread{},release_thread{};
} report;
template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
uintptr_t global_of(void* vm) noexcept {
    __try {return vm?read<uintptr_t>(reinterpret_cast<uintptr_t>(vm)+0x18):0;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {++read_failures;return 0;}
}
bool ours(void* vm,int index) noexcept {
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        const auto base=read<uintptr_t>(l+0x10),top=read<uintptr_t>(l+8);
        if(!base || top<base || (top-base)%24 || top-base>4096*24 || !index || index<=-10000) return false;
        const auto cells=(top-base)/24;
        if((index>0 && static_cast<uintptr_t>(index)>cells) ||
           (index<0 && static_cast<uintptr_t>(-int64_t(index))>cells)) return false;
        const auto value=index>0?base+uintptr_t(index-1)*24:top-static_cast<uintptr_t>(-int64_t(index))*24;
        if(read<uint32_t>(value+16)!=7) return false;
        const auto closure=read<uintptr_t>(value);
        if(!closure || read<uint8_t>(closure)!=7 || read<uint8_t>(closure+3)) return false;
        const auto proto=read<uintptr_t>(closure+0x18);
        if(!proto) return false;
        const auto source=read<uintptr_t>(proto+0x58);
        constexpr char label[]="=crml_persistent_events";
        return source && read<uint8_t>(source)==5 && read<uint32_t>(source+0x14)==sizeof(label)-1 &&
            !std::memcmp(reinterpret_cast<const void*>(source+0x18),label,sizeof(label)-1);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {++read_failures;return false;}
}
int retain_impl(void* vm,int index,uintptr_t caller) {
    // This call site belongs to event registration. CRML's own root reference
    // and unrelated game script references are never added to this ledger.
    const bool matched=enabled.load(std::memory_order_acquire) && caller==compatibility::address(image_base,0x1a15722) && ours(vm,index);
    const auto global=matched?global_of(vm):0;
    const int ref=original_retain(vm,index);
    if(matched && global && ref>0) {
        AcquireSRWLockExclusive(&gate);
        Ref* free=nullptr;bool duplicate=false;
        for(auto& slot:report.refs) {
            if(slot.global==global && slot.reference==ref) duplicate=true;
            if(!slot.global) free=&slot;
        }
        if(duplicate || !free) ++report.lost;
        else {*free={global,ref};++report.retained;report.retain_thread=GetCurrentThreadId();}
        ReleaseSRWLockExclusive(&gate);
    }
    return ref;
}
void release_impl(void* vm,int reference,uintptr_t caller) {
    bool matched=false;
    const auto global=enabled.load(std::memory_order_acquire) && reference>0?global_of(vm):0;
    if(global) {
        AcquireSRWLockExclusive(&gate);
        for(auto& slot:report.refs) if(slot.global==global && slot.reference==reference) {
            slot={};matched=true;++report.pending;break;
        }
        ReleaseSRWLockExclusive(&gate);
    }
    original_release(vm,reference);
    if(matched) {
        AcquireSRWLockExclusive(&gate);
        --report.pending;++report.released;report.release_thread=GetCurrentThreadId();
        if(caller==compatibility::address(image_base,0x1a08f15)) ++report.explicit_removals;
        else if(caller==compatibility::address(image_base,0x1a09648)) ++report.owner_removals;
        else if(caller==compatibility::address(image_base,0x1a160f1)) ++report.error_removals;
        else ++report.other_removals;
        ReleaseSRWLockExclusive(&gate);
    }
}
__declspec(noinline) int retain_hook(void* vm,int index) {
    return retain_impl(vm,index,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
__declspec(noinline) void release_hook(void* vm,int reference) {
    release_impl(vm,reference,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
}
bool start(uintptr_t image) noexcept {
#if defined(CRML_LUA_PROBE) || defined(CRML_LUA_SOURCE)
    if(!image || original_retain || original_release) return false;
    constexpr unsigned char a[]{0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20};
    constexpr unsigned char b[]{0x85,0xd2,0x7e,0x46,0x57,0x48,0x83,0xec,0x20};
    auto* retain_entry=reinterpret_cast<void*>(compatibility::address(image,0x2c507a0));
    auto* release_entry=reinterpret_cast<void*>(compatibility::address(image,0x2c508d0));
    if(!compatibility::matches_code(retain_entry,0x2c507a0,a,sizeof(a)) || !compatibility::matches_code(release_entry,0x2c508d0,b,sizeof(b))) return false;
    image_base=image;
    if(MH_CreateHook(retain_entry,reinterpret_cast<void*>(&retain_hook),reinterpret_cast<void**>(&original_retain))!=MH_OK ||
       MH_CreateHook(release_entry,reinterpret_cast<void*>(&release_hook),reinterpret_cast<void**>(&original_release))!=MH_OK ||
       MH_EnableHook(retain_entry)!=MH_OK || MH_EnableHook(release_entry)!=MH_OK) return false;
    enabled.store(true,std::memory_order_release);return true;
#else
    (void)image;return false;
#endif
}
void close(uintptr_t global) noexcept {
    if(!enabled.load()) return;
    AcquireSRWLockExclusive(&gate);
    for(auto& slot:report.refs) if(global && slot.global==global) {slot={};++report.vm_reclaimed;}
    if(!global) ++report.lost;
    ReleaseSRWLockExclusive(&gate);
}
void write(std::ostream& out) {
    Report copy;
    AcquireSRWLockShared(&gate);copy=report;ReleaseSRWLockShared(&gate);
    size_t active=0;for(const auto& ref:copy.refs) if(ref.global) ++active;
    out<<"{\"type\":\"lua_listener_refs\",\"schema\":1,\"enabled\":"<<(enabled.load()?"true":"false")
       <<",\"retained\":"<<copy.retained<<",\"released\":"<<copy.released<<",\"active\":"<<active
       <<",\"explicit_removals\":"<<copy.explicit_removals<<",\"owner_removals\":"<<copy.owner_removals
       <<",\"error_removals\":"<<copy.error_removals<<",\"other_removals\":"<<copy.other_removals
       <<",\"vm_reclaimed\":"<<copy.vm_reclaimed<<",\"pending\":"<<copy.pending<<",\"lost\":"<<copy.lost
       <<",\"read_failures\":"<<read_failures.load()<<",\"retain_thread\":"<<copy.retain_thread
       <<",\"release_thread\":"<<copy.release_thread<<"}\n";
}
#ifdef CRML_LUA_REFERENCES_TESTING
namespace testing {
void configure(Retain retain,Release release,uintptr_t image) {
    enabled=false;report={};read_failures=0;original_retain=retain;original_release=release;image_base=image;enabled=true;
}
int retain(void* vm,int index,uintptr_t caller) {return retain_impl(vm,index,caller);}
void release(void* vm,int ref,uintptr_t caller) {release_impl(vm,ref,caller);}
}
#endif
}
