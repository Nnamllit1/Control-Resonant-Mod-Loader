#include "controller_hook.h"
#include "compatibility.h"
#include <MinHook.h>
#include <atomic>
#ifdef CRML_CONTROLLER_TESTING
#include <stdexcept>
#endif

namespace crml::controller {
namespace {
Move original{};
uintptr_t installed{};
std::atomic<Movement> writer{};
std::atomic<Observer> observer{};
void dispatch(void* view,void* world,void* collision,void* callback,void* scene,void* time) {
    if(const auto fn=writer.load(std::memory_order_acquire)) fn(original,view,world,collision,callback,scene,time);
    else original(view,world,collision,callback,scene,time);
    // Observe the real view after the movement adapter has finished, never its
    // temporary replacement, and only after the original returns successfully.
    if(const auto fn=observer.load(std::memory_order_acquire)) fn(view,world);
}
}
bool start(uintptr_t image) noexcept {
    if(installed) return installed==image;
    if(!image) return false;
    auto* target=reinterpret_cast<void*>(compatibility::address(image,0x1b98950));
    constexpr unsigned char bytes[]{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x53,0x56,0x57};
    if(!compatibility::matches_code(target,0x1b98950,bytes,sizeof(bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&dispatch),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    installed=image;return true;
}
bool movement(Movement fn) noexcept {
    if(!fn) return false;
    Movement expected{};
    return writer.compare_exchange_strong(expected,fn) || expected==fn;
}
bool observe(Observer fn) noexcept {
    if(!fn) return false;
    Observer expected{};
    return observer.compare_exchange_strong(expected,fn) || expected==fn;
}
#ifdef CRML_CONTROLLER_TESTING
namespace {
unsigned sequence{}, originals{}, observations{};
bool intact{};
void fake(void* a,void* b,void* c,void* d,void* e,void* f) {
    ++originals; sequence=2;
    intact=a==reinterpret_cast<void*>(1) && b==reinterpret_cast<void*>(2) && c==reinterpret_cast<void*>(3)
        && d==reinterpret_cast<void*>(4) && e==reinterpret_cast<void*>(5) && f==reinterpret_cast<void*>(6);
}
void adapter(Move next,void* a,void* b,void* c,void* d,void* e,void* f) {
    sequence=1;next(a,b,c,d,e,f);sequence=3;
}
void sample(void* view,void* world) noexcept {
    ++observations;intact=intact && sequence==3 && view==reinterpret_cast<void*>(1) && world==reinterpret_cast<void*>(2);
}
}
bool test_dispatch() {
    original=&fake;
    dispatch(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6));
    bool ok=intact && originals==1 && observations==0;
    ok=ok && movement(&adapter) && movement(&adapter) && observe(&sample) && observe(&sample);
    ok=ok && !movement(+[](Move,void*,void*,void*,void*,void*,void*){}) && !observe(+[](void*,void*) noexcept {});
    dispatch(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6));
    ok=ok && intact && originals==2 && observations==1;
    original=+[](void*,void*,void*,void*,void*,void*) {throw std::runtime_error("engine error");};
    bool propagated=false;
    try {dispatch(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);}
    catch(const std::runtime_error&) {propagated=true;}
    return ok && propagated && observations==1;
}
#endif
}
