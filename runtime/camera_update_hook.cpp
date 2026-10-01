#include "camera_update_hook.h"
#include "compatibility.h"
#include <MinHook.h>
#include <atomic>
#ifdef CRML_CAMERA_SERVICE_TESTING
#include <stdexcept>
#endif

namespace crml::camera_update {
namespace {
Update original{};
uintptr_t installed{};
std::atomic<Adapter> adapter{};
std::atomic<Observer> observer{};
void dispatch(void* context,void* render,void* listener) {
    const auto sample=observer.load(std::memory_order_acquire);
    if(sample) sample(context,true);
    if(const auto fn=adapter.load(std::memory_order_acquire)) fn(original,context,render,listener);
    else original(context,render,listener);
    // An engine unwind is not a completed sample. It propagates unchanged and
    // deliberately leaves the snapshot provider unavailable for this process.
    if(sample) sample(context,false);
}
}
bool start(uintptr_t image) noexcept {
    if(installed) return installed==image;
    if(!image) return false;
    auto* target=reinterpret_cast<void*>(image+0x1baa070);
    constexpr unsigned char bytes[]{0x4c,0x8b,0xdc,0x49,0x89,0x73,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x90,0,0};
    if(!compatibility::matches(target,bytes,sizeof(bytes))) return false;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(&dispatch),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    installed=image;return true;
}
bool adapt(Adapter fn) noexcept {
    if(!fn) return false;
    Adapter expected{};return adapter.compare_exchange_strong(expected,fn) || expected==fn;
}
bool observe(Observer fn) noexcept {
    if(!fn) return false;
    Observer expected{};return observer.compare_exchange_strong(expected,fn) || expected==fn;
}
#ifdef CRML_CAMERA_SERVICE_TESTING
namespace {
unsigned sequence{},calls{},samples{};bool intact{};
void fake(void* a,void* b,void* c) {++calls;intact=a==reinterpret_cast<void*>(1) && b==reinterpret_cast<void*>(2) && c==reinterpret_cast<void*>(3);sequence=2;}
void around(Update next,void* a,void* b,void* c) {sequence=1;next(a,b,c);sequence=3;}
void sample(void*,bool before) noexcept {++samples;if(before) sequence=0;else intact&=sequence==3;}
}
bool test_dispatch() {
    original=&fake;dispatch(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3));
    bool ok=intact && calls==1 && samples==0;
    ok&=adapt(&around) && adapt(&around) && observe(&sample) && observe(&sample);
    ok&=!adapt(+[](Update,void*,void*,void*){}) && !observe(+[](void*,bool) noexcept {});
    dispatch(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3));
    ok&=intact && calls==2 && samples==2;
    original=+[](void*,void*,void*) {throw std::runtime_error("engine unwind");};
    bool threw=false;try {dispatch(nullptr,nullptr,nullptr);} catch(const std::runtime_error&) {threw=true;}
    return ok && threw && samples==3;
}
#endif
}
