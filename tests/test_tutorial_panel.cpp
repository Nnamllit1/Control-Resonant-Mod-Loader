#include "tutorial_panel.h"
#include "compatibility.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace crml::tutorial;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class T> void put(std::vector<unsigned char>& b,size_t offset,T value) {std::memcpy(b.data()+offset,&value,sizeof(value));}
uintptr_t address(const std::vector<unsigned char>& b) {return reinterpret_cast<uintptr_t>(b.data());}
struct Fixture;
thread_local Fixture* fixture{};
struct Fixture {
    static constexpr uintptr_t image=0x140000000;
    static constexpr uint64_t entity=(uint64_t{7}<<32)|2;
    static constexpr uint16_t system_id=23,archetype=1;
    static constexpr uint32_t row=2,key=42;
    std::vector<unsigned char> world=std::vector<unsigned char>(0x58600),system=std::vector<unsigned char>(0x150);
    std::vector<unsigned char> meta=std::vector<unsigned char>(16),generations=std::vector<unsigned char>(32),locations=std::vector<unsigned char>(32);
    std::vector<unsigned char> hashes=std::vector<unsigned char>(8),offsets=std::vector<unsigned char>(8),chunk=std::vector<unsigned char>(4096);
    std::vector<unsigned char> state=std::vector<unsigned char>(0x40),stacks=std::vector<unsigned char>(16),audio=std::vector<unsigned char>(16);
    std::vector<unsigned char> context=std::vector<unsigned char>(0xf0),stack_name=std::vector<unsigned char>(64),state_name=std::vector<unsigned char>(64);
    std::array<uint32_t,3> writes{0xbca2cd2b,0xc92b7ddf,0xddf9af37};
    std::array<uintptr_t,4> args{},query{};
    std::array<std::string_view,4> history{"pause","static_tutorial","options","static_tutorial"};
    uintptr_t world_view{};
    unsigned close_calls{},stack_calls{},first_calls{},active_calls{};
    bool is_active=true,fail_close=false,throw_close=false;
    void (*on_first)(Fixture&){};
    void (*on_stack)(Fixture&){};
    PanelNative native{};
    Fixture() {
        fixture=this;world_view=address(world);
        put(system,0,system_id);put(system,0x140,image+panel_dispatch_rva);put(system,0x148,image+panel_job_rva);
        for(auto offset:{0x133,0x134,0x136}) put(system,offset,uint8_t{1});
        put(system,0x48,reinterpret_cast<uintptr_t>(writes.data()));put(system,0x50,uint32_t{3});
        args={0,address(state),address(stacks),address(audio)};put(system,0x58,reinterpret_cast<uintptr_t>(args.data()));
        put(world,0x58478,address(meta));put(meta,8,uint64_t{2});
        put(world,0x58510,uint64_t{4});put(world,0x584e8,address(generations));put(generations,16,uint32_t{7});
        put(world,0x58530,address(locations));put(locations,16,(uint64_t{row}<<32)|archetype);
        put(world,0x58,address(chunk));put(world,0x1044c,row+1);put(chunk,0x10+row*8,entity);
        put(world,(archetype+0xc22)*32+0x10,address(hashes));put(world,(archetype+0xc22)*32+0x24,uint32_t{2});
        put(world,0x18478,address(offsets));
        put(hashes,0,uint32_t{0xae510289});put(hashes,4,uint32_t{0x48941389});
        put(offsets,0,uint32_t{0x100});put(offsets,4,uint32_t{0x400});
        query={address(chunk)+0x100,address(chunk)+0x400,address(chunk),row};
        put(state,0xc,key);put(state,0x10,uint8_t{1});put(state,0x14,uint8_t{1});put(state,0x30,uint8_t{1});
        put(context,0xbc,int32_t{1});put(context,0xc8,uint32_t{2});
        inline_name(stack_name,"game");inline_name(state_name,"static_tutorial");
        native={image,address(stack_name),address(state_name),get_stack,first,active,close};
    }
    static void inline_name(std::vector<unsigned char>& out,std::string_view text) {
        put(out,0,uint32_t{0x04000004});put(out,4,static_cast<uint32_t>(text.size()));
        std::memcpy(out.data()+8,text.data(),text.size());
    }
    PanelCall call() {return {query.data(),state.data(),stacks.data(),audio.data(),image+panel_event_return_rva};}
    PanelOwner owner() {return {{world_view,entity},address(state),key,1};}
    PanelClose attempt() {return PanelScope::close_owned(call(),owner());}
    static void* get_stack(const NativeStringView* name,void* stacks) {
        auto& f=*fixture;++f.stack_calls;
        if(f.on_stack) f.on_stack(f);
        return stacks==f.stacks.data() && std::string_view(name->data,name->size)=="game"?f.context.data():nullptr;
    }
    static int32_t first(const void* context,const NativeStringView* name) {
        auto& f=*fixture;++f.first_calls;
        if(context!=f.context.data()) return -1;
        uint32_t count{};std::memcpy(&count,f.context.data()+0xc8,4);
        int32_t found=-1;
        for(uint32_t i=0;i<count && i<f.history.size();++i)
            if(f.history[i]==std::string_view(name->data,name->size)) {found=static_cast<int32_t>(i);break;}
        if(f.on_first) f.on_first(f);
        return found;
    }
    static bool active(const void*,void*) {++fixture->active_calls;return fixture->is_active;}
    static void close(void* state,void* stacks) {
        auto& f=*fixture;++f.close_calls;
        require(state==f.state.data() && stacks==f.stacks.data(),"native close receives current environment objects");
        if(f.throw_close) throw std::runtime_error("native close C++ exception");
        put(f.context,0xbc,int32_t{0});put(f.state,0x14,uint8_t{0});
        if(f.fail_close) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
        // Native sync has not yet hidden the presentation or restored input.
    }
};
void scope_cases() {
    Fixture f;Identity id{123,456};
    require(f.attempt()==PanelClose::unavailable,"no close outside callback scope");
    require(PanelScope::identify(f.call(),id)==IdentityStatus::unavailable && !id.world && !id.entity,"no cached identity");
    PanelScope scope(&f.world_view,f.system_id,f.system.data(),f.native);
    require(PanelScope::identify(f.call(),id)==IdentityStatus::ok && id.world==f.world_view && id.entity==f.entity,"full entity round trip");
    auto c=f.call();c.caller=f.image+0x1e62d87;
    require(PanelScope::identify(c,id)==IdentityStatus::scope,"archetype caller is not a mutable environment boundary");
    c=f.call();c.audio=f.state.data();require(PanelScope::identify(c,id)==IdentityStatus::scope,"audio slot checked too");
    for(size_t i=0;i<2;++i) {
        f.query[i]+=16;require(PanelScope::identify(f.call(),id)==IdentityStatus::components,"both component arrays must match");f.query[i]-=16;
    }
    put(f.generations,16,uint32_t{8});require(f.attempt()==PanelClose::identity,"stale entity generation refused");put(f.generations,16,uint32_t{7});
    put(f.locations,16,(uint64_t{f.row+1}<<32)|f.archetype);require(f.attempt()==PanelClose::identity,"entity relocation refused");
    put(f.locations,16,(uint64_t{f.row}<<32)|f.archetype);
    const auto view=f.world_view;f.world_view=0;require(f.attempt()==PanelClose::scope,"world replacement invalidates scope");f.world_view=view;
    auto old=f.args[1];f.args[1]=f.args[2];require(f.attempt()==PanelClose::scope,"environment rebinding invalidates scope");f.args[1]=old;
    for(auto offset:{0x133,0x134,0x136}) {
        put(f.system,offset,uint8_t{0});require(f.attempt()==PanelClose::scope,"whole-system environment flags required");put(f.system,offset,uint8_t{1});
    }
    std::swap(f.writes[0],f.writes[2]);require(PanelScope::identify(f.call(),id)==IdentityStatus::ok,"environment declarations may be sorted");
    f.writes[0]=f.writes[1];require(f.attempt()==PanelClose::scope,"missing environment write permission refused");f.writes[0]=0xddf9af37;
    {
        PanelScope nested(nullptr,0,nullptr,f.native);
        require(f.attempt()==PanelClose::scope,"invalid nested callback shadows outer permission");
    }
    require(PanelScope::identify(f.call(),id)==IdentityStatus::ok,"outer scope restored");
    PanelClose other_thread{};std::thread thread([&] {other_thread=f.attempt();});thread.join();
    require(other_thread==PanelClose::unavailable,"scope cannot transfer to runtime worker");
    require(!f.close_calls && !f.stack_calls,"rejected context never invokes engine stack helpers");
}
void guard_cases() {
    Fixture f;PanelScope scope(&f.world_view,f.system_id,f.system.data(),f.native);
    auto owner=f.owner();++owner.key;
    require(PanelScope::close_owned(f.call(),owner)==PanelClose::foreign,"cannot close game-owned ID");
    owner=f.owner();owner.mode=0;require(PanelScope::close_owned(f.call(),owner)==PanelClose::foreign,"ID equality does not equate primary and secondary record");
    owner=f.owner();++owner.state;require(PanelScope::close_owned(f.call(),owner)==PanelClose::identity,"registration environment must match");
    owner=f.owner();++owner.identity.entity;require(PanelScope::close_owned(f.call(),owner)==PanelClose::identity,"registration entity must match");
    for(uint8_t kind=0;kind<=4;++kind) {
        put(f.state,4,kind);put(f.state,8,uint8_t{1});require(f.attempt()==PanelClose::pending,"all pending native events defer close, including unkeyed events");
    }
    put(f.state,8,uint8_t{0});put(f.state,0x14,uint8_t{0});require(f.attempt()==PanelClose::inactive,"inactive is not retirement acknowledgement");
    put(f.state,0x14,uint8_t{1});f.is_active=false;require(f.attempt()==PanelClose::inactive,"use full native context activity predicate");f.is_active=true;
    for(auto index:{-1,0,2,3}) {
        put(f.context,0xbc,int32_t{index});require(f.attempt()==PanelClose::not_top,"invalid/root/forward index is not closable");
    }
    put(f.context,0xc8,uint32_t{4});put(f.context,0xbc,int32_t{2});
    require(f.attempt()==PanelClose::not_top,"visible ancestor cannot pop Options");
    put(f.context,0xbc,int32_t{3});require(f.attempt()==PanelClose::not_top,"duplicate name earlier in stack cannot pop intervening menus");
    put(f.context,0xbc,int32_t{1});f.history[1]="options";
    require(f.attempt()==PanelClose::not_top,"target only in forward history is not current");f.history[1]="static_tutorial";
    put(f.context,0xc8,uint32_t{4097});require(f.attempt()==PanelClose::not_top,"bounded native stack scan");put(f.context,0xc8,uint32_t{4});
    require(!f.close_calls,"all refusals leave native close uncalled");
    const auto chunk=f.chunk;
    require(f.attempt()==PanelClose::await_sync && f.close_calls==1,"top-owned panel requests native close");
    require(f.state[0x14]==0 && f.state[0x30]==1 && f.chunk==chunk,"await native presentation/input sync; leave component and progression data intact");
    require(f.attempt()==PanelClose::inactive && f.close_calls==1,"no repeat close or invented completion acknowledgement");
    put(f.state,0x14,uint8_t{1});put(f.context,0xbc,int32_t{1});put(f.state,0x10,uint8_t{0});
    owner=f.owner();owner.mode=0;
    require(PanelScope::close_owned(f.call(),owner)==PanelClose::await_sync && f.close_calls==2,"primary close does not enqueue native completion");
}
void change_cases() {
    Fixture f;PanelScope scope(&f.world_view,f.system_id,f.system.data(),f.native);
    f.on_first=+[](Fixture& v) {put(v.state,8,uint8_t{1});};
    require(f.attempt()==PanelClose::changed,"pending event appearing during reads defers close");put(f.state,8,uint8_t{0});
    f.on_first=+[](Fixture& v) {put(v.state,0xc,uint32_t{99});};
    require(f.attempt()==PanelClose::changed,"current panel replacement defers close");put(f.state,0xc,f.key);
    f.on_first=+[](Fixture& v) {v.state_name[8]='x';};
    require(f.attempt()==PanelClose::changed,"canonical target changed during lookup");f.state_name[8]='s';
    f.on_first=+[](Fixture& v) {put(v.generations,16,uint32_t{8});};
    require(f.attempt()==PanelClose::changed,"generation changes during helper calls rechecked");put(f.generations,16,uint32_t{7});
    f.on_first=nullptr;
    f.stack_calls=0;f.on_stack=+[](Fixture& v) {if(v.stack_calls==2) put(v.state,8,uint8_t{1});};
    require(f.attempt()==PanelClose::changed,"last native lookup cannot bypass pending-state recheck");
    require(!f.close_calls,"changed snapshots never mutate native state");
}
void open_cases() {
    Fixture f;
    require(!PanelScope::open_owned(f.call(),f.owner()),"open requires dispatcher scope");
    PanelScope scope(&f.world_view,f.system_id,f.system.data(),f.native);
    require(!PanelScope::open_owned(f.call(),f.owner()),"open defers existing active presentation");
    put(f.state,0x14,uint8_t{0});
    require(!PanelScope::open_owned(f.call(),f.owner()),"open waits for prior native input sync");
    put(f.state,0x30,uint8_t{0});put(f.state,8,uint8_t{1});
    const auto pending=f.state;
    require(!PanelScope::open_owned(f.call(),f.owner()) && f.state==pending,"open preserves foreign pending event");
    put(f.state,8,uint8_t{0});auto owner=f.owner();++owner.identity.entity;
    require(!PanelScope::open_owned(f.call(),owner),"open refuses stale entity generation");
    owner=f.owner();owner.mode=0;
    require(!PanelScope::open_owned(f.call(),owner),"adapter opens secondary only");
    const auto chunk=f.chunk;
    require(PanelScope::open_owned(f.call(),f.owner()),"owned secondary publication accepted");
    uint32_t key{};std::memcpy(&key,f.state.data(),4);
    require(key==f.key && f.state[4]==1 && f.state[8]==1 && !f.state[0x14] && f.chunk==chunk,
        "publish native variant without forging active state or mutating ECS");
    require(!PanelScope::open_owned(f.call(),f.owner()),"do not overwrite unconsumed open");
    require(!PanelScope::release_owned(f.call(),f.owner()),"retirement does not clear pending input");
    put(f.state,8,uint8_t{0});put(f.state,0x30,uint8_t{1});
    require(!PanelScope::release_owned(f.call(),f.owner()),"retirement waits for native sync");
    put(f.state,0x30,uint8_t{0});
    require(PanelScope::release_owned(f.call(),f.owner()),"retire inactive synchronized owned identifier");
    std::memcpy(&key,f.state.data()+0xc,4);
    require(!key && f.state[0x10]==0xff,"private identifier no longer remains in native state");
}
void memory_cases() {
    Fixture f;PanelScope scope(&f.world_view,f.system_id,f.system.data(),f.native);
    put(f.stack_name,4,uint32_t{257});require(f.attempt()==PanelClose::invalid,"canonical string length bounded");put(f.stack_name,4,uint32_t{4});
    const char heap_name[]="game";
    put(f.stack_name,0,uint32_t{0x01000004});put(f.stack_name,8,reinterpret_cast<uintptr_t>(heap_name));
    require(f.attempt()==PanelClose::await_sync,"heap-backed native canonical string decoded");
    put(f.state,0x14,uint8_t{1});put(f.context,0xbc,int32_t{1});
    auto* memory=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);require(memory!=nullptr,"guard allocation");
    DWORD previous{};const bool protected_ok=VirtualProtect(memory,4096,PAGE_READWRITE|PAGE_GUARD,&previous)!=0;
    put(f.stack_name,8,reinterpret_cast<uintptr_t>(memory));const auto guarded=f.attempt();
    MEMORY_BASIC_INFORMATION info{};const bool preserved=VirtualQuery(memory,&info,sizeof(info)) && (info.Protect&PAGE_GUARD);
    VirtualFree(memory,0,MEM_RELEASE);require(protected_ok && guarded==PanelClose::invalid && preserved && f.close_calls==1,"guarded name refused without consuming PAGE_GUARD");
    Fixture::inline_name(f.stack_name,"game");f.fail_close=true;
    require(f.attempt()==PanelClose::partial && f.close_calls==2 && f.state[0x14]==0,"native close fault after mutation requires reconciliation, never reports retired");
}
constexpr DWORD custom_exception=0xe043524d;
bool invoke_seh(Fixture& f) {
    __try {
        PanelScope::invoke(&f.world_view,f.system_id,f.system.data(),f.native,
            +[](const void*,uint16_t,const void*) {RaiseException(custom_exception,0,0,nullptr);});
    } __except(GetExceptionCode()==custom_exception?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
void unwind_cases() {
    Fixture f;bool caught=false;
    try {
        PanelScope::invoke(&f.world_view,f.system_id,f.system.data(),f.native,+[](const void* view,uint16_t id,const void* system) {
            auto& f=*fixture;require(view==&f.world_view && id==f.system_id && system==f.system.data(),"dispatcher arguments forwarded");
            f.throw_close=true;f.attempt();
        });
    } catch(const std::runtime_error&) {caught=true;}
    require(caught && f.attempt()==PanelClose::unavailable,"C++ unwind removes borrowed context");
    require(invoke_seh(f) && f.attempt()==PanelClose::unavailable,"recovered native SEH cannot leave stale TLS scope under EHsc");
    PanelScope outer(&f.world_view,f.system_id,f.system.data(),f.native);
    require(invoke_seh(f),"nested native SEH propagated");Identity id{};
    require(PanelScope::identify(f.call(),id)==IdentityStatus::ok,"outer context restored after nested native exception");
}
}
int main() {
    try {
        PanelNative native{};native.image=123;
        require(!PanelNative::bind(0,native) && !native.image,"unsupported native binding clears output");
        scope_cases();guard_cases();change_cases();open_cases();memory_cases();unwind_cases();
        std::cout<<"tutorial panel scope, close guards, preservation and unwind checks passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
