#include "visibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace crml::probe::visibility {
namespace {
using Apply = void(*)(void*);
using Allocate = void*(*)(void*,uint64_t,bool);
using Wake = void(*)(void*);
Apply original{};
Player player_callback{};
uintptr_t image{};
std::atomic<bool> ready{};
std::atomic<uint64_t> sent{};
Lease lease;

template<class T> T read(uintptr_t p) noexcept { T v; std::memcpy(&v,reinterpret_cast<void*>(p),sizeof(v)); return v; }
bool resolve_inner(const void* query,const Sample& p,Target& out) noexcept {
    if(!query || p.rejection!=Rejection::none || !p.world || !p.entity) return false;
    const auto q=reinterpret_cast<uintptr_t>(query);
    if(read<uintptr_t>(q)!=p.world) return false;
    uintptr_t chunk{}; uint32_t row{};
    const auto hidden=entity_component(p.world,p.entity,0xb1f2545a,1,chunk,row);
    if(!hidden || row!=p.row) return false;
    const auto location=read<uint64_t>(read<uintptr_t>(p.world+0x58530)+uint32_t(p.entity)*8ull);
    const auto arch=uint16_t(location);
    const auto words=read<uint64_t>(q+8), count=read<uint64_t>(q+24);
    const auto bits=read<uintptr_t>(q+16);
    if(!bits || count>8192 || arch>=count || words!=(count+63)/64 ||
       !(read<uint64_t>(bits+(arch/64)*8ull)&(uint64_t{1}<<(arch%64)))) return false;
    uintptr_t flags_chunk{}, render_chunk{}; uint32_t flags_row{},render_row{};
    const auto flags=entity_component(p.world,p.entity,0x77d4f0ad,4,flags_chunk,flags_row);
    const auto render=entity_component(p.world,p.entity,0x0eee2128,24,render_chunk,render_row);
    if(!flags || !render || flags_chunk!=chunk || render_chunk!=chunk || flags_row!=row || render_row!=row) return false;
    if(read<uint8_t>(hidden)>1) return false;
    const auto handle=read<uint32_t>(render+16);
    if(handle==UINT32_MAX) return false;
    out={reinterpret_cast<uint8_t*>(hidden),reinterpret_cast<uint32_t*>(flags),handle};
    return true;
}
void apply(void* query) {
    // Original processes game hide reasons first, including restoration after release.
    original(query);
    if(!ready.load(std::memory_order_acquire) || !lease.active(GetTickCount64())) return;
    const auto p=player_callback();
    Target target{};
    if(!resolve(query,p,target) || *target.hidden) return;
    // Same small command path as applyHide. No guest supplies addresses or handles.
    const auto renderer=*reinterpret_cast<uintptr_t*>(image+0x5e69000);
    if(!renderer) return;
    const auto storage=*reinterpret_cast<uintptr_t*>(renderer+24);
    if(!storage) return;
    auto* command=static_cast<Command*>(reinterpret_cast<Allocate>(image+0x2fb8340)(reinterpret_cast<void*>(storage),8,true));
    if(!command) return;
    *command=hide_command(target.handle);
    *target.flags |= 0x10000;
    *target.hidden=1;
    // Publish the record exactly as the engine does; the low bit requests wakeup.
    *reinterpret_cast<volatile uint64_t*>(reinterpret_cast<uintptr_t>(command)-8)=8;
    auto* counter=reinterpret_cast<volatile LONG*>(storage+0x30);
    if(InterlockedExchangeAdd(counter,2)&1) reinterpret_cast<Wake>(image+0x393109d)(const_cast<LONG*>(counter));
    sent.fetch_add(1,std::memory_order_relaxed);
}
}
bool resolve(const void* query,const Sample& p,Target& out) noexcept {
    out={}; bool ok=false;
    __try { ok=resolve_inner(query,p,out); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { ok=false; }
    if(!ok) out={};
    return ok;
}
bool start(uintptr_t base,Player player) noexcept {
    if(!base || !player) return false;
    const unsigned char expected[]={0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    auto* target=reinterpret_cast<void*>(base+0x1969ca0);
    if(std::memcmp(target,expected,sizeof(expected))) return false;
    image=base; player_callback=player;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&apply),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    if(MH_EnableHook(target)!=MH_OK) { MH_RemoveHook(target); return false; }
    ready.store(true,std::memory_order_release); return true;
}
void stop() noexcept { lease.deadline.store(0); ready.store(false); }
int poll(uint64_t owner,bool held,uint64_t now) noexcept {
    if(!ready.load() || !owner) return -1;
    return lease.renew(owner,held,now);
}
void release(uint64_t owner) noexcept { lease.release(owner); }

uint64_t submissions() noexcept { return sent.load(); }
}
