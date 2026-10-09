#include "tutorial_panel.h"
#include "compatibility.h"
#include "movement_view.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <limits>

namespace crml::tutorial {
namespace {
thread_local const PanelScope* current{};
template<class T> T read(uintptr_t address) noexcept {
    T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value;
}
bool extent(uintptr_t start,size_t bytes) noexcept {
    return start && bytes && start<=std::numeric_limits<uintptr_t>::max()-bytes;
}
bool accessible(uintptr_t start,size_t bytes,bool write=false) noexcept {
    if(!extent(start,bytes)) return false;
    const auto end=start+bytes;
    while(start<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(start),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto p=info.Protect&0xff;
        const bool writable=p==PAGE_READWRITE || p==PAGE_WRITECOPY || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
        if(!writable && (write || (p!=PAGE_READONLY && p!=PAGE_EXECUTE_READ))) return false;
        const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(!extent(base,info.RegionSize) || base+info.RegionSize<=start) return false;
        start=base+info.RegionSize;
    }
    return true;
}
struct Environment {uintptr_t world{},state{},stacks{},audio{};};
bool environment(const void* view,uint16_t id,const void* system,uintptr_t image,Environment& out) noexcept {
    __try {
        const auto s=reinterpret_cast<uintptr_t>(system),v=reinterpret_cast<uintptr_t>(view);
        if(!extent(image,0x6301000) || !accessible(s,0x150) || !accessible(v,8)) return false;
        if(read<uint16_t>(s)!=id || read<uintptr_t>(s+0x140)!=image+panel_dispatch_rva ||
           read<uintptr_t>(s+0x148)!=image+panel_job_rva) return false;
        // Mutable environments force whole-system scheduling. Their writer
        // conflicts are independent of the per-entity query filters.
        if(!read<uint8_t>(s+0x133) || !read<uint8_t>(s+0x134) || !read<uint8_t>(s+0x136) || read<uint32_t>(s+0x50)!=3) return false;
        const auto hashes=read<uintptr_t>(s+0x48),args=read<uintptr_t>(s+0x58);
        if(!accessible(hashes,12) || !accessible(args,32)) return false;
        constexpr uint32_t required[]{0xbca2cd2b,0xc92b7ddf,0xddf9af37};
        for(const auto hash:required) {
            unsigned matches=0;for(unsigned i=0;i<3;++i) matches+=read<uint32_t>(hashes+i*4)==hash;
            if(matches!=1) return false;
        }
        out={read<uintptr_t>(v),read<uintptr_t>(args+8),read<uintptr_t>(args+16),read<uintptr_t>(args+24)};
        return accessible(out.world,0x58600) && accessible(out.state,0x40) && accessible(out.stacks,8) && accessible(out.audio,8) &&
            out.state!=out.stacks && out.state!=out.audio && out.stacks!=out.audio;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
IdentityStatus resolve(uintptr_t world,const void* query,Identity& out) noexcept {
    __try {
        if(!accessible(reinterpret_cast<uintptr_t>(query),32)) return IdentityStatus::query;
        std::array<uintptr_t,4> words{};std::memcpy(words.data(),query,sizeof(words));
        const auto chunk=words[2],row=words[3];
        if(row>=16384 || !accessible(chunk,0x10+(row+1)*8)) return IdentityStatus::query;
        const auto entity=read<uint64_t>(chunk+0x10+row*8);
        if(!entity || entity==UINT64_MAX) return IdentityStatus::entity;
        constexpr uint32_t hashes[]{0xae510289,0x48941389},strides[]{0x70,16};
        for(unsigned i=0;i<2;++i) {
            uintptr_t actual_chunk{};uint32_t actual_row{};
            if(!extent(words[i],(row+1)*strides[i])) return IdentityStatus::query;
            const auto component=probe::entity_component(world,entity,hashes[i],strides[i],actual_chunk,actual_row);
            if(!component || actual_chunk!=chunk || actual_row!=row || component!=words[i]+row*strides[i]) return IdentityStatus::components;
        }
        uintptr_t final_chunk{};uint32_t final_row{};
        if(probe::entity_component(world,entity,hashes[0],strides[0],final_chunk,final_row)!=words[0]+row*strides[0] ||
           final_chunk!=chunk || final_row!=row || std::memcmp(words.data(),query,sizeof(words)) || read<uint64_t>(chunk+0x10+row*8)!=entity)
            return IdentityStatus::changed;
        out={world,entity};return IdentityStatus::ok;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return IdentityStatus::memory;}
}
struct Name {
    NativeStringView view{};
    std::array<char,256> text{};
};
bool name(uintptr_t object,Name& out) noexcept {
    if(!accessible(object,16)) return false;
    const auto packed=read<uint32_t>(object),length=read<uint32_t>(object+4);
    const auto storage=(packed&0xffffff)*8,code=packed>>24;
    const auto capacity=code<0x80?code*8:(code<<8)-0x7c00;
    if(!length || length>out.text.size()) return false;
    const auto data=storage>capacity?read<uintptr_t>(object+8):object+8;
    if(!accessible(data,length)) return false;
    out.view={reinterpret_cast<const char*>(data),length};std::memcpy(out.text.data(),out.view.data,length);
    return true;
}
bool same(const Name& a,const Name& b) noexcept {
    return a.view.data==b.view.data && a.view.size==b.view.size && a.text==b.text;
}
struct State {
    uint32_t key{};uint8_t mode{},active{},pending{};
};
State state(uintptr_t p) noexcept {return {read<uint32_t>(p+0xc),read<uint8_t>(p+0x10),read<uint8_t>(p+0x14),read<uint8_t>(p+8)};}
bool same(State a,State b) noexcept {return a.key==b.key && a.mode==b.mode && a.active==b.active && a.pending==b.pending;}
}
bool PanelNative::bind(uintptr_t image,PanelNative& out) noexcept {
    out={};
    if(!compatibility::reviewed_build || compatibility::engine_profile!=compatibility::EngineProfile::october_patch ||
       image!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) || !extent(image,0x6301000)) return false;
    struct Site {uintptr_t rva;std::array<unsigned char,32> bytes;};
    constexpr Site sites[]{
        {panel_dispatch_rva,{72,137,92,36,16,85,86,87,65,84,65,85,65,86,65,87,72,131,236,112,77,139,208,72,139,1,65,15,183,144,200,0}},
        {0x1e63f30,{0x40,0x53,0x48,0x83,0xec,0x40,0x8b,0x05,0x9c,0xdb,0xe0,0x03,0x4c,0x8b,0xd2,0x25,0xff,0xff,0xff,0x00,0x48,0x8b,0xd9,0x44,0x8d,0x04,0xc5,0x00,0x00,0x00,0x00,0x0f}},
        {0x17d8cf0,{72,137,92,36,16,87,72,131,236,48,72,139,2,72,139,24,139,64,8,72,105,248,240,0,0,0,72,3,251,72,59,223}},
        {0x17d8170,{72,137,92,36,8,72,137,108,36,16,72,137,116,36,24,72,137,124,36,32,65,86,72,131,236,16,139,153,200,0,0,0}},
        {0x17d8680,{72,139,196,72,137,88,16,72,137,104,24,72,137,112,32,87,65,84,65,85,65,86,65,87,72,131,236,80,76,139,226,76}}
    };
    for(const auto& site:sites)
        if(!compatibility::matches(reinterpret_cast<void*>(image+site.rva),site.bytes.data(),site.bytes.size())) return false;
    const auto* call=reinterpret_cast<const unsigned char*>(image+panel_event_return_rva-5);
    const int32_t relative=0x1e60d50-static_cast<int32_t>(panel_event_return_rva);
    if(!compatibility::matches(call,"\xe8",1) || !compatibility::matches(call+1,&relative,4)) return false;
    out={image,image+0x5c71ad8,image+0x5c71e10,
         reinterpret_cast<decltype(stack)>(image+0x17d8cf0),reinterpret_cast<decltype(first)>(image+0x17d8170),
         reinterpret_cast<decltype(active)>(image+0x17d8680),reinterpret_cast<decltype(close)>(image+0x1e63f30)};
    return true;
}
PanelScope::PanelScope(const void* view,uint16_t id,const void* system,const PanelNative& native) noexcept
    :previous_(current),world_view_(view),system_(system),native_(native),id_(id) {
    Environment e{};
    if(environment(view,id,system,native.image,e)) {world_=e.world;state_=e.state;stacks_=e.stacks;audio_=e.audio;}
    current=this;
}
void PanelScope::leave() noexcept {if(current==this) current=previous_;}
PanelScope::~PanelScope() {leave();}
void PanelScope::forward(PanelScope& scope,Callback callback) {
    __try {callback(scope.world_view_,scope.id_,scope.system_);}
    __finally {scope.leave();}
}
void PanelScope::invoke(const void* view,uint16_t id,const void* system,const PanelNative& native,Callback callback) {
    PanelScope scope(view,id,system,native);forward(scope,callback);
}
bool PanelScope::valid(const PanelCall* call) const noexcept {
    Environment e{};
    if(!world_ || !environment(world_view_,id_,system_,native_.image,e) ||
       e.world!=world_ || e.state!=state_ || e.stacks!=stacks_ || e.audio!=audio_) return false;
    return !call || (call->caller==native_.image+panel_event_return_rva &&
        reinterpret_cast<uintptr_t>(call->state)==state_ && reinterpret_cast<uintptr_t>(call->stacks)==stacks_ &&
        reinterpret_cast<uintptr_t>(call->audio)==audio_);
}
IdentityStatus PanelScope::identify(const PanelCall& call,Identity& out) noexcept {
    out={};const auto* scope=current;
    if(!scope) return IdentityStatus::unavailable;
    if(!scope->valid(&call)) return IdentityStatus::scope;
    Identity candidate{};const auto result=resolve(scope->world_,call.query,candidate);
    if(result!=IdentityStatus::ok) return result;
    if(!scope->valid(&call)) return IdentityStatus::changed;
    out=candidate;return IdentityStatus::ok;
}
PanelClose PanelScope::close_owned(const PanelCall& call,const PanelOwner& owner) {
    const auto* scope=current;
    if(!scope) return PanelClose::unavailable;
    if(!scope->valid(&call)) return PanelClose::scope;
    Identity identity{};
    if(identify(call,identity)!=IdentityStatus::ok || !owner.identity.world || !owner.identity.entity ||
       identity.world!=owner.identity.world || identity.entity!=owner.identity.entity || owner.state!=scope->state_) return PanelClose::identity;
    const auto& native=scope->native_;
    if(!owner.key || owner.mode>1 || !native.stack || !native.first || !native.active || !native.close) return PanelClose::invalid;
    bool issuing=false;
    __try {
        const auto before=state(scope->state_);
        if(before.key!=owner.key || before.mode!=owner.mode) return PanelClose::foreign;
        // Even close/page-next pending events carry no owner identifier. Never
        // clear them or assume they belong to this registration.
        if(before.pending) return PanelClose::pending;
        if(!before.active) return PanelClose::inactive;
        Name stack_name{},state_name{};
        if(!name(native.stack_name,stack_name) || !name(native.state_name,state_name)) return PanelClose::invalid;
        auto* context=native.stack(&stack_name.view,call.stacks);
        const auto p=reinterpret_cast<uintptr_t>(context);
        if(!accessible(p,0xf0)) return PanelClose::invalid;
        const auto count=read<uint32_t>(p+0xc8);const auto index=read<int32_t>(p+0xbc);
        // Index zero cannot be popped. A visible ancestor or a duplicate name
        // earlier in history would pop unrelated menus through the helper.
        if(!count || count>4096 || index<=0 || static_cast<uint32_t>(index)>=count) return PanelClose::not_top;
        if(!native.active(context,call.stacks)) return PanelClose::inactive;
        if(native.first(context,&state_name.view)!=index) return PanelClose::not_top;
        Name final_stack{},final_state{};Identity final_identity{};
        // Finish native lookups before the last identity/state reads. A
        // reentrant lookup must not introduce an unchecked pending event.
        if(native.stack(&stack_name.view,call.stacks)!=context) return PanelClose::changed;
        if(current!=scope || identify(call,final_identity)!=IdentityStatus::ok || final_identity.world!=identity.world ||
           final_identity.entity!=identity.entity || !same(before,state(scope->state_)) ||
           !name(native.stack_name,final_stack) || !name(native.state_name,final_state) ||
           !same(stack_name,final_stack) || !same(state_name,final_state) ||
           read<uint32_t>(p+0xc8)!=count || read<int32_t>(p+0xbc)!=index)
            return PanelClose::changed;
        if(!accessible(scope->state_+0x14,1,true)) return PanelClose::invalid;
        issuing=true;native.close(call.state,call.stacks);
        // Neither this call nor active=false establishes renderer retirement,
        // input restoration or permission to release a registered record.
        return PanelClose::await_sync;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return issuing?PanelClose::partial:PanelClose::invalid;
    }
}
bool PanelScope::open_owned(const PanelCall& call,const PanelOwner& owner) noexcept {
    Identity identity{};
    const auto* scope=current;
    if(!scope || !owner.key || owner.mode!=1 || owner.state!=scope->state_ ||
       identify(call,identity)!=IdentityStatus::ok || identity.world!=owner.identity.world ||
       identity.entity!=owner.identity.entity) return false;
    __try {
        const auto p=scope->state_;
        if(!accessible(p,0x40,true) || read<uint8_t>(p+8) || read<uint8_t>(p+0x14) || read<uint8_t>(p+0x30)) return false;
        if(!scope->valid(&call)) return false;
        std::memcpy(reinterpret_cast<void*>(p),&owner.key,4);
        *reinterpret_cast<uint8_t*>(p+4)=1;
        *reinterpret_cast<uint8_t*>(p+8)=1;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool PanelScope::release_owned(const PanelCall& call,const PanelOwner& owner) noexcept {
    Identity identity{};const auto* scope=current;
    if(!scope || owner.mode!=1 || owner.state!=scope->state_ || identify(call,identity)!=IdentityStatus::ok ||
       identity.world!=owner.identity.world || identity.entity!=owner.identity.entity) return false;
    __try {
        const auto p=scope->state_;
        if(!accessible(p,0x40,true) || read<uint8_t>(p+8) || read<uint8_t>(p+0x14) || read<uint8_t>(p+0x30)) return false;
        // A displaced native state is not ours to rewrite; it no longer carries
        // this private key. The caller has already observed completed sync.
        if(read<uint32_t>(p+0xc)!=owner.key || read<uint8_t>(p+0x10)!=1) return true;
        if(!scope->valid(&call)) return false;
        const uint32_t empty=0;std::memcpy(reinterpret_cast<void*>(p+0xc),&empty,4);
        *reinterpret_cast<uint8_t*>(p+0x10)=0xff;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
}
