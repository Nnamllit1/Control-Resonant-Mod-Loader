#include "lua_probe.h"
#include "lua_session.h"
#include "lua_probe_bytecode.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <map>
#include <string>

using namespace crml::probe::lua;
namespace {
constexpr uintptr_t image=0x10000,site=image+0x1a0aadf;
std::array<unsigned char,256> vm{},context{};
std::array<unsigned char,4096> global{},stack{},moved{};
std::array<unsigned char,0x58600> world{};
std::array<uint64_t,2> generations{};
std::array<unsigned char,40> ci{};
std::array<unsigned char,600> error_string{};
int calls{},loads{},protects{},restores{},mode{};
bool deliberate{};
bool in_body{};
using Cell=std::array<unsigned char,24>;
std::map<uint64_t,std::map<std::string,Cell>> tables;
uint64_t next_table{};
bool persistent_loading{},persistent_error{};
bool persistent_event_mode{};
bool persistent_rollback{};
int listener_shutdowns{};
int listener_error_initializations{};
int rollback_initializations{};
uint64_t persistent_environment{};
int persistent_counter{},retains{},fetches{},releases{};
std::map<int,Cell> registry;
constexpr uint64_t owner_id=0x200000001;
Owner owner() {return {reinterpret_cast<uintptr_t>(world.data()),owner_id};}
template<class T> void put(void* p,size_t offset,T value) {std::memcpy(static_cast<unsigned char*>(p)+offset,&value,sizeof(value));}
template<class T> T get(void* p,size_t offset) {T v;std::memcpy(&v,static_cast<unsigned char*>(p)+offset,sizeof(v));return v;}
void require(bool value,const char* text) {if(!value) {std::cerr<<text<<'\n';std::exit(1);}}
void push(int tag,double value=0) {
    auto top=get<uintptr_t>(vm.data(),8);
    put(reinterpret_cast<void*>(top),0,value);put(reinterpret_cast<void*>(top),16,tag);
    put(vm.data(),8,top+24);
}
void push_error(const char* message) {
    error_string={};put(error_string.data(),0,uint8_t{5});
    put(error_string.data(),0x14,static_cast<uint32_t>(std::strlen(message)));
    std::memcpy(error_string.data()+0x18,message,std::strlen(message));
    const auto top=get<uintptr_t>(vm.data(),8);push(5);
    put(reinterpret_cast<void*>(top),0,reinterpret_cast<uintptr_t>(error_string.data()));
}
Cell cell(int index) {
    Cell v{};
    if(index==-10002) {put(v.data(),0,uint64_t{1});put(v.data(),16,int{6});return v;}
    const auto at=index>0?get<uintptr_t>(vm.data(),0x10)+(index-1)*24:get<uintptr_t>(vm.data(),8)+index*24;
    std::memcpy(v.data(),reinterpret_cast<void*>(at),24);return v;
}
void push_cell(Cell v) {
    const auto top=get<uintptr_t>(vm.data(),8);push(0);
    std::memcpy(reinterpret_cast<void*>(top),v.data(),24);
}
void new_table(void*,int array,int hash) {
    require(array==0 && (hash==3 || hash==1),"table allocation sizes");
    if(mode==12) throw 4;
    Cell v{};put(v.data(),0,++next_table);put(v.data(),16,int{6});push_cell(v);
}
void push_value(void*,int index) {push_cell(cell(index));}
void set_field(void*,int index,const char* key) {
    auto table=cell(index),value=cell(-1);
    tables[get<uint64_t>(table.data(),0)][key]=value;
    put(vm.data(),8,get<uintptr_t>(vm.data(),8)-24);
}
void readonly(void*,int index,int flag) {
    require(index==-1 && flag==1 && get<int>(cell(-1).data(),16)==6,"readonly metatable");
}
int set_metatable(void* l,int index) {set_field(l,index,"__test_meta");return 1;}
int raw_field(void*,int index,const char* key) {
    auto table=cell(index);auto value=tables[get<uint64_t>(table.data(),0)][key];
    push_cell(value);return get<int>(value.data(),16);
}
int load(void* l,const char* label,const char* bytes,size_t size,int env) {
    if(std::strstr(label,"crml_persistent")) {
        require(l==vm.data() && env==2 && size>2 && bytes[0]==6 && bytes[1]==3,"persistent loader environment and bytecode");
        persistent_loading=true;persistent_error=std::strstr(label,"error")!=nullptr;
        persistent_event_mode=std::strstr(label,"_events")!=nullptr;
        persistent_environment=get<uint64_t>(cell(env).data(),0);persistent_counter=0;
        require(get<uint64_t>(tables[persistent_environment]["self"].data(),0)==owner_id,"persistent owner in private environment");
        push(7);return 0;
    }
    require(l==vm.data() && env==(loads==4?2:0) && size>2 && bytes[0]==6 && bytes[1]==3,"loader ABI/bytecode");
    if(env) {
        require(get<uint64_t>(cell(env).data(),0)==2 && get<uint64_t>(tables[2]["_ENV"].data(),0)==2,"rooted private environment");
        require(get<uint64_t>(tables[2]["self"].data(),0)==owner_id && get<uint64_t>(tables[3]["__index"].data(),0)==1,"owner and global inheritance");
    }
    ++loads;deliberate=std::strstr(label,"error")!=nullptr;
    after_call(vm.data(),site,1,0,1,0); // Reentrancy must not start another run.
    if(mode==1) {push(5);return 1;}
    if(mode==2) throw 4;
    push(7);return 0;
}
int call(void* l,int args,int results,int err) {
    if(persistent_loading || !registry.empty()) {
        require(l==vm.data() && (args==0 || (persistent_event_mode && (args==1 || (persistent_loading && (args==2 || args==3))))) && results==1 && err==0,"persistent call ABI");
        if(persistent_loading && persistent_event_mode) {
            persistent_error=args==1;
            persistent_rollback=args==3;
            if(args) require(get<int>(cell(-1).data(),16)==6,"error-stage constructor flag is a rooted table");
            if(args==2) {
                ++listener_error_initializations;
                require(get<int>(cell(-2).data(),16)==6 && cell(-1)==cell(-2),"listener-error flags are the same rooted environment");
            }
            if(args==3) {
                ++rollback_initializations;
                require(cell(-1)==cell(-2) && cell(-2)==cell(-3),"rollback flags use the same rooted environment");
            }
        } else if(args) require(get<int>(cell(-1).data(),16)==7,"shutdown argument is rooted closure duplicate");
        put(vm.data(),8,get<uintptr_t>(vm.data(),8)-(args+1)*24);
        if(persistent_loading) {persistent_loading=false;push(7);return 0;}
        if(args) {++listener_shutdowns;push(3,mode==16?-401:persistent_counter);return 0;}
        ++persistent_counter;
        if(persistent_rollback && persistent_counter==1) {push(3,-410);return 0;}
        if(persistent_error && persistent_counter==3) {
            const auto message=std::string(persistent_event_mode?"crml_persistent_events:":"crml_persistent_error:")+
                std::to_string(persistent_event_mode?bytecode::persistent_events_error_line:bytecode::persistent_error_error_line)+": attempt to call a nil value";
            push_error(message.c_str());return 2;
        }
        push(3,persistent_counter);return 0;
    }
    require(l==vm.data() && args==(calls==4?1:0) && results==(calls==4?2:1) && err==0,"protected call ABI");
    ++calls;
    if(calls==5 && mode!=10) {
        Cell marker{};put(marker.data(),0,double{42});put(marker.data(),16,int{3});
        tables[2]["__crml_probe_environment_v1"]=marker;
        if(mode==11) tables[1]["__crml_probe_environment_v1"]=marker;
    }
    put(vm.data(),8,get<uintptr_t>(vm.data(),8)-(args+1)*24);
    if(deliberate) push_error("crml_probe_error:3: attempt to call a nil value");
    else if(calls==5 && mode>=5 && mode<=8) {
        if(mode==5) push_error("crml_probe_events:29: no active script");
        if(mode==6) push_error("private_asset_name:31: unrelated internal diagnostic");
        if(mode==7) push(2);
        if(mode==8) {push_error("crml_probe_events:12: required ECS environment missing");put(error_string.data(),0x14,uint32_t{5000});}
        return 2;
    } else {
        push(3,calls==5 && mode==9?-201:calls>=4?127:42);
        if(results==2) {
            if(mode==9) push_error("crml_probe_events:42: required ECS environment missing");
            else push(0);
        }
    }
    if(mode==3) {
        const auto old=get<uintptr_t>(vm.data(),0x30);
        const auto top=get<uintptr_t>(vm.data(),8)-old;
        const auto base=get<uintptr_t>(vm.data(),0x10)-old;
        std::memcpy(moved.data(),reinterpret_cast<void*>(old),stack.size());
        put(vm.data(),0x30,reinterpret_cast<uintptr_t>(moved.data()));
        put(vm.data(),8,reinterpret_cast<uintptr_t>(moved.data())+top);
        put(vm.data(),0x10,reinterpret_cast<uintptr_t>(moved.data())+base);
        put(vm.data(),0x28,reinterpret_cast<uintptr_t>(moved.data()+moved.size()-24));
        put(ci.data(),0,reinterpret_cast<uintptr_t>(moved.data())+base);
        mode=0;
    }
    if(mode==4) put(stack.data(),24,uint64_t{123}); // Corrupt a pre-existing value.
    return deliberate?2:0;
}
int protect(void* l,Body body,void* user,ptrdiff_t top,ptrdiff_t err) {
    require(l==vm.data() && top==48 && err==0,"barrier receives saved stack offset");
    ++protects;
    try {in_body=true;body(l,user);in_body=false;return 0;} catch(int status) {
        in_body=false;
        put(vm.data(),8,get<uintptr_t>(vm.data(),0x30)+top);push(5);return status;
    }
}
void settop(void* l,int n) {
    require(l==vm.data() && n>=1 && n<=4,"bounded API stack index");
    if(!in_body) {require(n==1,"restore original stack");++restores;}
    put(l,8,get<uintptr_t>(l,0x10)+n*24);
}
void push_entity(void* l,uint64_t id,int tag) {
    require(l==vm.data() && id==owner_id && tag==1,"owner push ABI");
    auto top=get<uintptr_t>(l,8);push(2);
    put(reinterpret_cast<void*>(top),0,id);put(reinterpret_cast<void*>(top),8,tag);
}
int retain(void*,int index) {
    require(index==-1 && get<int>(cell(index).data(),16)==7,"retain returned callback");
    if(mode==13) throw 4;
    registry[++retains]=cell(index);return retains;
}
int fetch(void*,int index,int reference) {
    require(index==-10000 && registry.contains(reference),"fetch only owned live registry reference");
    ++fetches;
    if(mode==15) {push(3);return 3;}
    push_cell(registry.at(reference));return 7;
}
void release(void*,int reference) {
    require(registry.contains(reference),"reference released exactly once");
    ++releases;registry.erase(reference);
    if(mode==14) throw 4; // Even an ambiguous release may have recycled the slot.
}
void reset(int m=0) {
    vm={};context={};global={};stack={};moved={};ci={};world={};generations={0,2};
    calls=loads=protects=restores=0;mode=m;deliberate=false;
    in_body=false;tables.clear();next_table=1;
    persistent_loading=persistent_error=false;persistent_environment=0;persistent_counter=retains=fetches=releases=0;registry.clear();
    persistent_event_mode=persistent_rollback=false;listener_shutdowns=listener_error_initializations=rollback_initializations=0;
    auto ptr=[](auto& a) {return reinterpret_cast<uintptr_t>(a.data());};
    put(vm.data(),8,ptr(stack)+48);put(vm.data(),0x10,ptr(stack)+24);
    put(vm.data(),0x18,ptr(global));put(vm.data(),0x20,ptr(ci));
    put(vm.data(),0x28,ptr(stack)+stack.size()-24);put(vm.data(),0x30,ptr(stack));
    put(vm.data(),0x40,ptr(ci));put(vm.data(),0x58,uintptr_t{0x1234});
    put(vm.data(),0x78,ptr(context));put(context.data(),0,ptr(world));
    put(world.data(),0x584e8,ptr(generations));put(world.data(),0x58510,uint64_t{2});
    put(ci.data(),0,ptr(stack)+24);put(stack.data(),24+16,uint32_t{7});
    configure({&load,&protect,&settop,&call,&push_entity,&new_table,&push_value,&set_field,&readonly,&set_metatable,&raw_field,&retain,&fetch,&release},image);
}
std::string output() {std::ostringstream out;write(out);return out.str();}
void invoke() {after_call(vm.data(),site,1,0,1,0,owner());}
}
int main() {
    reset();
    after_call(vm.data(),site+1,1,0,1,0);after_call(vm.data(),site,0,0,1,0);
    after_call(vm.data(),site,1,1,1,0);after_call(vm.data(),site,1,0,0,0);
    after_call(vm.data(),site,1,0,1,2);
    require(loads==0,"only the reviewed successful call site may run");
    invoke();require(loads==5 && calls==5 && protects==5 && restores==5,"all five phases");
    require(output().find("\"passed\":true")!=std::string::npos,"successful report");
    require(output().find("\"value\":127")!=std::string::npos,"capability result");
    require(output().find("\"error_kind\":\"nil_call\",\"error_line\":3")!=std::string::npos,"expected error classified before cleanup");
    invoke();require(loads==5,"once per process");
    reset();put(vm.data(),0x28,reinterpret_cast<uintptr_t>(stack.data()+144));invoke();
    require(loads==5 && restores==5 && output().find("\"passed\":true")!=std::string::npos,"cleanup accepts results occupying reserved stack slots");
    reset();for(int i=0;i<60;++i) push(0);
    const auto crowded=stack;
    invoke();require(loads==0 && protects==0 && stack==crowded,"reserve snapshot capacity for environment and results before execution");
    reset(1);invoke();require(loads==1 && calls==0 && restores==1,"load error cleaned, remaining phases stopped");
    require(output().find("\"passed\":false")!=std::string::npos,"load failure report");
    reset(2);invoke();require(loads==1 && restores==1,"allocation exception contained and cleaned");
    require(output().find("\"protect\":4")!=std::string::npos,"barrier failure report");
    reset(3);invoke();require(loads==5 && restores==5,"stack relocation handled by offsets");
    require(output().find("\"passed\":true")!=std::string::npos,"relocation report");
    reset(4);invoke();require(loads==1,"pre-existing payload corruption stops test");
    require(output().find("\"restored\":false")!=std::string::npos,"corruption reported");
    reset(5);invoke();require(output().find("\"error_kind\":\"no_active_script\",\"error_line\":29")!=std::string::npos,"event error category and owned source line");
    reset(6);invoke();auto foreign=output();
    require(foreign.find("private_asset_name")==std::string::npos && foreign.find("unrelated internal diagnostic")==std::string::npos,"never print arbitrary error text");
    require(foreign.find("\"error_kind\":\"other\",\"error_line\":0")!=std::string::npos,"foreign label has no source line");
    reset(7);invoke();require(output().find("\"error_kind\":\"unavailable\"")!=std::string::npos,"non-string error rejected");
    reset(8);invoke();require(output().find("\"error_kind\":\"unavailable\"")!=std::string::npos,"oversize error rejected");
    reset(9);invoke();auto caught=output();
    require(caught.find("\"value\":-201")!=std::string::npos && caught.find("\"error_kind\":\"missing_ecs\",\"error_line\":42")!=std::string::npos,"caught event error classified from second result");
    require(restores==5 && caught.find("\"passed\":false")!=std::string::npos,"diagnostic return is a restored failure, not success");
    reset(10);invoke();require(output().find("\"environment_created\":true,\"environment_verified\":false")!=std::string::npos,"missing private marker fails isolation check");
    reset(11);invoke();require(output().find("\"environment_created\":true,\"environment_verified\":false")!=std::string::npos,"shared marker write fails isolation check");
    reset(12);invoke();require(loads==4 && restores==5 && output().find("\"protect\":4")!=std::string::npos,"environment allocation error contained before script load");
    reset();put(global.data(),0xd28,uintptr_t{1});invoke();require(loads==0,"debugger suspension rejected");
    reset();put(vm.data(),3,uint8_t{1});invoke();require(loads==0,"suspended VM rejected");
    reset();put(vm.data(),0x78,uintptr_t{});invoke();require(loads==0,"missing world context rejected");
    reset();after_call(reinterpret_cast<void*>(1),site,1,0,1,0);require(loads==0,"unreadable VM rejected");
    reset();generations[1]=3;invoke();require(loads==0,"replaced owner rejected");
    reset();auto wrong=owner();wrong.world=0;after_call(vm.data(),site,1,0,1,0,wrong);require(loads==0,"foreign world rejected");
    reset();push(7);push_entity(vm.data(),owner_id,1);
    const auto saved=stack;const auto captured=before_call(vm.data(),site,1,0,1);
    require(captured.world==owner().world && captured.entity==owner_id && stack==saved,"read-only incoming owner capture");
    require(before_call(vm.data(),site+1,1,0,1).entity==0,"no capture from other callers");
    put(stack.data(),72+8,uint32_t{4});require(before_call(vm.data(),site,1,0,1).entity==0,"wrong userdata kind rejected");
    put(stack.data(),72+8,uint32_t{1});generations[1]=3;
    require(before_call(vm.data(),site,1,0,1).entity==0,"stale incoming owner rejected");
    reset();stop();invoke();require(loads==0,"stopped probe stays passive");
    reset();invoke();configure_persistent();
    session::Context session_context{vm.data(),reinterpret_cast<uintptr_t>(global.data()),owner().world,owner_id};
    const auto original_stack=stack;
    for(uint64_t time=0;time<=2500;time+=250) session::tick(session_context,time);
    require(retains==3 && fetches==6 && releases==2 && registry.size()==1,"real provider retains across calls and releases explicit/error sessions");
    require(stack[24]==original_stack[24] && get<uintptr_t>(vm.data(),8)==reinterpret_cast<uintptr_t>(stack.data()+48),"persistent operations restore original stack");
    std::ostringstream persistent_report;session::write(persistent_report);
    require(persistent_report.str().find("\"expected_errors\":1")!=std::string::npos && persistent_report.str().find("\"failures\":0")!=std::string::npos,"delayed error is contained and next session loads");
    session::cleanup_begin(session_context.global,owner_id);session::cleanup_end();
    session_context.revision=session::revision();session::tick(session_context,2750);
    require(releases==3 && registry.empty(),"retired owner callback released on later valid context");
    session::stop();
    reset();invoke();configure_persistent();session_context.revision=session::revision();mode=13;session::tick(session_context,0);
    require(registry.empty() && retains==0 && get<uintptr_t>(vm.data(),8)==reinterpret_cast<uintptr_t>(stack.data()+48),"retain allocation error contained and stack restored");
    reset();invoke();configure_persistent();session::tick(session_context,0);mode=15;session::tick(session_context,250);mode=0;session::tick(session_context,500);
    require(fetches==1 && releases==1 && registry.empty(),"wrong registry value rejected and owned reference retired");
    reset();invoke();configure_persistent();session::tick(session_context,0);mode=14;session::stop();session::tick(session_context,250);session::tick(session_context,500);
    require(releases==1 && registry.empty(),"ambiguous native release is not repeated");
    reset();invoke();configure_persistent(true);session_context.revision=session::revision();
    for(uint64_t time=0;time<=2500;time+=250) session::tick(session_context,time);
    require(retains==3 && fetches==8 && releases==2 && listener_shutdowns==2,"event provider shuts down explicit and failed sessions before releasing root");
    session::cleanup_begin(session_context.global,owner_id);session::cleanup_end();session_context.revision=session::revision();
    session::tick(session_context,2750);
    require(releases==3 && listener_shutdowns==2 && registry.empty(),"engine-owned retirement never calls shutdown with invalid listener handle");
    reset();invoke();configure_persistent(true);session_context.revision=session::revision();session::tick(session_context,0);
    mode=16;session::stop();session::tick(session_context,250);session::tick(session_context,500);
    require(releases==1 && registry.empty() && listener_shutdowns==1,"failed event-removal verification releases root and stops without retry");
    reset();invoke();configure_persistent(true);session_context.revision=session::revision();
    for(uint64_t time=0;time<=4000;time+=250) session::tick(session_context,time);
    require(listener_error_initializations==1 && retains==4 && releases==3 && listener_shutdowns==3 && registry.size()==1,"listener-error source mode completes before fresh session");
    std::ostringstream listener_report;session::write(listener_report);
    require(listener_report.str().find("\"listener_error_checks\":1")!=std::string::npos && listener_report.str().find("\"failures\":0")!=std::string::npos,"listener sequence records check without treating dispatch as controller error");
    for(uint64_t time=4250;time<=5500;time+=250) session::tick(session_context,time);
    std::ostringstream rollback_report;session::write(rollback_report);
    require(rollback_initializations==1 && retains==5 && releases==4 && listener_shutdowns==4 && registry.size()==1,"rollback controller recovers and unloads before final session");
    require(rollback_report.str().find("\"initialization_rollbacks\":1")!=std::string::npos && rollback_report.str().find("\"initialization_recoveries\":1")!=std::string::npos && rollback_report.str().find("\"failures\":0")!=std::string::npos,"rollback and retry record independent evidence");
    std::cout<<"Lua probe checks passed\n";
}
