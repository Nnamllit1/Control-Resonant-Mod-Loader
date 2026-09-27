#include "boundary_guard.h"
#include "script_origin.h"
#include <MinHook.h>
#include <atomic>
#include <ostream>

namespace crml::probe::boundary {
namespace {
using ProtectedCall=int(*)(void*,int,int,int);
ProtectedCall original{};
fall::Active active_callback{};
std::atomic<bool> ready{};
std::atomic<uint64_t> boundary_exits{},invalid_areas{},height_checks{},read_failures{};
std::atomic<uint64_t> flight_calls{};

// Match only the two entry handlers, before they schedule work or apply fog.
// Timed callbacks, update_transition and all cleanup handlers run normally.
unsigned handler(uintptr_t value) noexcept {
    using script_origin::detail::read;
    if(read<uint32_t>(value+0x10)!=7) return 0;
    const auto closure=read<uintptr_t>(value);
    if(!closure || read<uint8_t>(closure)!=7 || read<uint8_t>(closure+3)) return 0;
    const auto proto=read<uintptr_t>(closure+0x18);
    if(!proto) return 0;
    const auto line=read<uint32_t>(proto+0xa4),id=read<uint32_t>(proto+0xa8),words=read<uint32_t>(proto+0x88);
    const unsigned kind=line==77 && id==5 && words==90 ? 1 : line==105 && id==7 && words==34 ? 2 : 0;
    if(!kind) return 0;
    std::array<char,96> source{};std::array<char,64> name{};
    if(!script_origin::detail::label(read<uintptr_t>(proto+0x58),source,true) ||
       !script_origin::detail::label(read<uintptr_t>(proto+0x60),name,false)) return 0;
    return std::strcmp(source.data(),"out_of_bounds_area.lua")==0 &&
           std::strcmp(name.data(),kind==1?"on_exit_oob":"on_invalid_area_enter")==0 ? kind : 0;
}
bool omit(void* state,int nargs,int results,int error,fall::Player player) noexcept {
    if(!state || !player.world || !player.entity) return false;
    ++flight_calls;
    if(nargs<0 || nargs>8 || results!=0 || error!=1 || !fall::available(player)) return false;
    __try {
        using script_origin::detail::read;
        const auto vm=reinterpret_cast<uintptr_t>(state);
        if(read<uint8_t>(vm+3)) return false; // Never handle a suspended/error VM.
        const auto context=read<uintptr_t>(vm+0x78);
        if(!context || read<uintptr_t>(context)!=player.world) return false;
        const auto top=read<uintptr_t>(vm+8),base=read<uintptr_t>(vm+0x10);
        const auto stack=read<uintptr_t>(vm+0x30),last=read<uintptr_t>(vm+0x28);
        const auto bytes=(static_cast<uintptr_t>(nargs)+1)*0x18;
        if(!stack || base<stack || top<base || top>last || top-base<bytes+0x18 ||
           (base-stack)%0x18 || (top-base)%0x18) return false;
        const auto function=top-bytes;
        // errfunc=1 refers to the existing error handler at base. Leave it intact.
        if(read<uint32_t>(base+0x10)!=7) return false;
        const auto kind=handler(function);
        if(!kind) return false;
        // Successful zero-result protected calls remove the function and its
        // arguments. No callee frame was entered, so no frame/upvalue teardown
        // is needed. Preserve error handler, call frames and all stack payloads.
        std::memcpy(reinterpret_cast<void*>(vm+8),&function,sizeof(function));
        if(kind==1) ++boundary_exits;else ++invalid_areas;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        ++read_failures;return false;
    }
}
int protected_call(void* vm,int nargs,int results,int error) {
    if(ready.load(std::memory_order_acquire) && omit(vm,nargs,results,error,active_callback())) return 0;
    return original(vm,nargs,results,error);
}
}
bool start(uintptr_t image,fall::Active active) noexcept {
    if(!image || !active || active_callback) return false;
    constexpr unsigned char signature[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57};
    auto* entry=reinterpret_cast<void*>(image+0x2c4fc90);
    if(std::memcmp(entry,signature,sizeof(signature))) return false;
    active_callback=active;
    if(MH_CreateHook(entry,reinterpret_cast<void*>(&protected_call),reinterpret_cast<void**>(&original))!=MH_OK || MH_EnableHook(entry)!=MH_OK) return false;
    ready.store(true,std::memory_order_release);return true;
}
bool skip_height(const void* view) noexcept {
    if(!ready.load(std::memory_order_acquire)) return false;
    const auto p=active_callback();
    if(!p.entity || !fall::matches_inactive(view,p) || !fall::available(p)) return false;
    ++height_checks;return true;
}
void write(std::ostream& out) {
    out<<"{\"type\":\"boundary_guard\",\"tick_ms\":"<<GetTickCount64()<<",\"ready\":"<<(ready.load()?"true":"false")
       <<",\"script_exits_skipped\":"<<boundary_exits.load()<<",\"invalid_area_entries_skipped\":"<<invalid_areas.load()
       <<",\"height_checks_skipped\":"<<height_checks.load()<<",\"calls_during_flight\":"<<flight_calls.load()<<",\"read_failures\":"<<read_failures.load()<<"}\n";
}
void stop() noexcept {ready.store(false,std::memory_order_release);}
#ifdef CRML_FALL_TRACE_TESTING
namespace testing {
void configure(fall::Active active,void* call) noexcept {
    stop();active_callback=active;original=reinterpret_cast<ProtectedCall>(call);
    boundary_exits=0;invalid_areas=0;height_checks=0;read_failures=0;flight_calls=0;
    ready.store(true,std::memory_order_release);
}
int invoke(void* vm,int nargs,int results,int error) {return protected_call(vm,nargs,results,error);}
}
#endif
}
