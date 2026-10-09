#include "navigation_context.h"
#include "compatibility.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace crml::navigation_context {
namespace {
bool supported() noexcept {
    return compatibility::reviewed_build &&
        compatibility::engine_profile==compatibility::EngineProfile::october_patch;
}
template<class T> T load(uintptr_t at) noexcept {return *reinterpret_cast<const volatile T*>(at);}
bool range(uintptr_t p,size_t size) noexcept {return p && p<=(std::numeric_limits<uintptr_t>::max)()-size;}
Status bundle_inner(uintptr_t slot,Bundle& out) noexcept {
    if(!range(slot,8)) return Status::arguments;
    const auto object=load<uintptr_t>(slot);
    if(!range(object,0xa0)) return Status::arguments;
    const auto valid=load<uint8_t>(object+0x98);
    if(valid>1) return Status::malformed;
    const auto value=valid?load<uint64_t>(object+0x90):0;
    if(load<uintptr_t>(slot)!=object || load<uint8_t>(object+0x98)!=valid ||
       (valid && load<uint64_t>(object+0x90)!=value)) return Status::changed;
    out={value,valid!=0};return Status::ok;
}
Status source_inner(uintptr_t view,SavedTransform& out) noexcept {
    if(!range(view,0x110)) return Status::arguments;
    const auto base=load<uintptr_t>(view+0x78),row=load<uint64_t>(view+0x88);
    const auto slot=load<uintptr_t>(view+0x108);
    // A rejected oversized row is diagnostic loss, never a speculative walk.
    if(row>65535 || !range(base,static_cast<size_t>(row*32+32))) return Status::arguments;
    Bundle first{};auto status=bundle_inner(slot,first);if(status!=Status::ok)return status;
    SavedTransform candidate{};std::memcpy(candidate.transform,reinterpret_cast<void*>(base+row*32),32);
    for(float f:candidate.transform) if(!std::isfinite(f)) return Status::malformed;
    candidate.bundle=first.valid?first.value:0;
    candidate.bundle_valid=first.valid;
    Bundle second{};status=bundle_inner(slot,second);if(status!=Status::ok)return status;
    if(first.valid!=second.valid || first.value!=second.value ||
       load<uintptr_t>(view+0x78)!=base || load<uint64_t>(view+0x88)!=row ||
       load<uintptr_t>(view+0x108)!=slot ||
       std::memcmp(candidate.transform,reinterpret_cast<void*>(base+row*32),32))return Status::changed;
    out=candidate;return Status::ok;
}
Status restore_inner(uintptr_t saved,uintptr_t view,RestoreObservation& out) noexcept {
    if(!range(saved,40) || !range(view,0x211)) return Status::arguments;
    const auto slot=load<uintptr_t>(view+0xd8);
    const auto blocked=load<uint8_t>(view+0x210);
    if(blocked>1) return Status::malformed;
    Bundle current{};auto status=bundle_inner(slot,current);if(status!=Status::ok)return status;
    RestoreObservation candidate{};
    std::memcpy(candidate.saved.transform,reinterpret_cast<const void*>(saved),32);
    candidate.saved.bundle=load<uint64_t>(saved+32);
    candidate.saved.bundle_valid=candidate.saved.bundle!=0;
    candidate.current=current;
    candidate.context_disabled=blocked!=0;
    for(float f:candidate.saved.transform) if(!std::isfinite(f)) return Status::malformed;
    Bundle second{};status=bundle_inner(slot,second);if(status!=Status::ok)return status;
    if(load<uintptr_t>(view+0xd8)!=slot || load<uint8_t>(view+0x210)!=blocked ||
       load<uint64_t>(saved+32)!=candidate.saved.bundle ||
       std::memcmp(candidate.saved.transform,reinterpret_cast<const void*>(saved),32) ||
       current.valid!=second.valid || current.value!=second.value) return Status::changed;
    if(candidate.context_disabled) candidate.reason=RestoreReason::context_disabled;
    else if(!candidate.saved.bundle) candidate.reason=RestoreReason::missing_saved_bundle;
    else if(!current.valid) candidate.reason=RestoreReason::missing_current_bundle;
    else if(candidate.saved.bundle!=current.value) candidate.reason=RestoreReason::bundle_mismatch;
    else candidate.reason=RestoreReason::eligible;
    out=candidate;return Status::ok;
}
}
Status inspect_bundle(const void* pointer,Bundle& out) noexcept {
    out={};if(!supported())return Status::unavailable;
    Status status{};
    __try {status=bundle_inner(reinterpret_cast<uintptr_t>(pointer),out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {status=Status::memory;}
    if(status!=Status::ok)out={};return status;
}
Status inspect_save_source(const void* view,SavedTransform& out) noexcept {
    out={};if(!supported())return Status::unavailable;
    Status status{};
    __try {status=source_inner(reinterpret_cast<uintptr_t>(view),out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {status=Status::memory;}
    if(status!=Status::ok)out={};return status;
}
Status inspect_save_result(const void* destination,const SavedTransform& expected) noexcept {
    if(!supported())return Status::unavailable;
    if(!range(reinterpret_cast<uintptr_t>(destination),40))return Status::arguments;
    __try {return std::memcmp(destination,expected.transform,32)==0 &&
        load<uint64_t>(reinterpret_cast<uintptr_t>(destination)+32)==expected.bundle?Status::ok:Status::mismatch;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return Status::memory;}
}
Status inspect_restore_source(const void* saved,const void* view,RestoreObservation& out) noexcept {
    out={};if(!supported())return Status::unavailable;
    Status status{};
    __try {status=restore_inner(reinterpret_cast<uintptr_t>(saved),reinterpret_cast<uintptr_t>(view),out);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {status=Status::memory;}
    if(status!=Status::ok)out={};return status;
}
const char* name(Status status) noexcept {
    switch(status) {
    case Status::ok:return "ok";case Status::arguments:return "arguments";
    case Status::memory:return "memory";case Status::changed:return "changed";
    case Status::malformed:return "malformed";case Status::mismatch:return "mismatch";
    default:return "unavailable";
    }
}
const char* name(RestoreReason reason) noexcept {
    switch(reason) {
    case RestoreReason::eligible:return "eligible";
    case RestoreReason::context_disabled:return "context_disabled";
    case RestoreReason::missing_saved_bundle:return "missing_saved_bundle";
    case RestoreReason::missing_current_bundle:return "missing_current_bundle";
    default:return "bundle_mismatch";
    }
}
}
