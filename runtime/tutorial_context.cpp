#include "tutorial_context.h"
#include "movement_view.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <limits>

namespace crml::tutorial {
namespace {
thread_local const RequestScope* current{};
template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
// Reject wrapping addresses before reads. SEH handles inaccessible pages; it
// does not provide synchronization with another engine thread.
bool extent(uintptr_t start,uintptr_t bytes) noexcept {
    return start && bytes && start<=std::numeric_limits<uintptr_t>::max()-bytes;
}
uintptr_t scope_world(const void* view,uint16_t id,const void* system,Dispatch dispatch,uintptr_t image) noexcept {
    __try {
        const auto s=reinterpret_cast<uintptr_t>(system),v=reinterpret_cast<uintptr_t>(view);
        if(!extent(s,0x150) || !extent(v,8) || !extent(image,0x63e2000)) return 0;
        if(dispatch!=Dispatch::direct && dispatch!=Dispatch::archetype) return 0;
        if(read<uintptr_t>(s+0x140)!=image+request_dispatch_rva ||
           read<uintptr_t>(s+0x148)!=image+request_job_rva) return 0;
        if(dispatch==Dispatch::direct && read<uint16_t>(s)!=id) return 0;
        if(dispatch==Dispatch::archetype && id>=8192) return 0;
        const auto world=read<uintptr_t>(v);
        return extent(world,0x58600)?world:0;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return 0;}
}
IdentityStatus resolve(uintptr_t world,const void* query,Dispatch dispatch,uint16_t id,Identity& out,std::array<uintptr_t,7>* components=nullptr) noexcept {
    // Read-only component access contract, in native aggregate order.
    constexpr uint32_t hashes[]{0xed2c12b8,0x48941389,0x12707f01,0x9fc946e4,0xae510289,0x4a56ea3c,0x423c371a};
    constexpr uint32_t strides[]{16,16,16,16,0x70,16,16};
    __try {
        if(!extent(reinterpret_cast<uintptr_t>(query),9*8)) return IdentityStatus::query;
        std::array<uintptr_t,9> words{};
        std::memcpy(words.data(),query,sizeof(words));
        const auto chunk=words[7],row=words[8];
        if(row>=16384 || !extent(chunk,0x10+(row+1)*8)) return IdentityStatus::query;
        const auto entity=read<uint64_t>(chunk+0x10+row*8);
        if(!entity || entity==UINT64_MAX) return IdentityStatus::entity;
        for(size_t i=0;i<7;++i) {
            if(!extent(words[i],(row+1)*strides[i])) return IdentityStatus::query;
            uintptr_t resolved_chunk{};uint32_t resolved_row{};
            const auto component=probe::entity_component(world,entity,hashes[i],strides[i],resolved_chunk,resolved_row);
            if(!component || resolved_chunk!=chunk || resolved_row!=row || component!=words[i]+row*strides[i])
                return IdentityStatus::components;
        }
        const auto locations=read<uintptr_t>(world+0x58530);
        const auto index=static_cast<uint32_t>(entity);
        if(!extent(locations,(static_cast<uintptr_t>(index)+1)*8)) return IdentityStatus::entity;
        if(dispatch==Dispatch::archetype && static_cast<uint16_t>(read<uint64_t>(locations+index*8ull))!=id)
            return IdentityStatus::scope;
        // Repeat the round trip and query snapshot before publishing identity.
        // This rejects observed changes; it cannot prove absence of an ABA race.
        uintptr_t final_chunk{};uint32_t final_row{};
        const auto data=probe::entity_component(world,entity,hashes[4],strides[4],final_chunk,final_row);
        if(!data || final_chunk!=chunk || final_row!=row || data!=words[4]+row*strides[4] ||
           std::memcmp(words.data(),query,sizeof(words)) || read<uint64_t>(chunk+0x10+row*8)!=entity)
            return IdentityStatus::changed;
        if(components) for(size_t i=0;i<7;++i) (*components)[i]=words[i]+row*strides[i];
        out={world,entity};return IdentityStatus::ok;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return IdentityStatus::memory;}
}
}
RequestScope::RequestScope(const void* view,uint16_t id,const void* system,Dispatch dispatch,uintptr_t image) noexcept
    :previous_(current),world_view_(view),system_(system),image_(image),
     world_(scope_world(view,id,system,dispatch,image)),id_(id),dispatch_(dispatch) {current=this;}
void RequestScope::leave() noexcept {if(current==this) current=previous_;}
RequestScope::~RequestScope() {leave();}
void RequestScope::forward(RequestScope& scope,Callback callback) {
    __try {callback(scope.world_view_,scope.id_,scope.system_);}
    __finally {scope.leave();}
}
void RequestScope::invoke(const void* view,uint16_t id,const void* system,Dispatch dispatch,uintptr_t image,Callback callback) {
    RequestScope scope(view,id,system,dispatch,image);
    forward(scope,callback);
}
IdentityStatus RequestScope::identify(const void* query,Identity& out) noexcept {
    out={};const auto* scope=current;
    if(!scope) return IdentityStatus::unavailable;
    if(!scope->world_ || scope_world(scope->world_view_,scope->id_,scope->system_,scope->dispatch_,scope->image_)!=scope->world_)
        return IdentityStatus::scope;
    Identity candidate{};
    const auto status=resolve(scope->world_,query,scope->dispatch_,scope->id_,candidate);
    if(status!=IdentityStatus::ok) return status;
    if(scope_world(scope->world_view_,scope->id_,scope->system_,scope->dispatch_,scope->image_)!=scope->world_)
        return IdentityStatus::changed;
    out=candidate;return IdentityStatus::ok;
}
Withdrawal RequestScope::withdraw(const void* query,Identity expected,uint32_t owned_key) noexcept {
    const auto* scope=current;
    if(!scope || !scope->world_ || scope_world(scope->world_view_,scope->id_,scope->system_,scope->dispatch_,scope->image_)!=scope->world_)
        return {WithdrawalStatus::scope};
    Identity identity{};std::array<uintptr_t,7> components{};
    if(!expected.world || !expected.entity || resolve(scope->world_,query,scope->dispatch_,scope->id_,identity,&components)!=IdentityStatus::ok ||
       identity.world!=expected.world || identity.entity!=expected.entity ||
       scope_world(scope->world_view_,scope->id_,scope->system_,scope->dispatch_,scope->image_)!=scope->world_)
        return {WithdrawalStatus::identity};
    return withdraw_queued(components,owned_key);
}
}
