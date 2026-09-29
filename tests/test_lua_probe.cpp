#include "lua_probe.h"
#include "lua_session.h"
#include "lua_probe_bytecode.h"
#include "lua_executor.h"
#include "lua_controller.h"
#ifdef CRML_LUA_SOURCE
#include "lua_source.h"
#include "lua_compile.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#endif
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
int lifecycle_phase{},lifecycle_kind{};
bool closed_vm{};
std::map<int,Cell> registry;
namespace native=crml::engine::lua;
// Sentinel bytes belong to the fake loader only. They deliberately differ from
// every embedded probe; this fixture does not claim to validate VM bytecode.
constexpr unsigned char custom_bytes[]{6,3,0xad,0x71,0x08};
bool custom_mode{},custom_loading{},custom_owner_live{true},custom_vm_live{true};
int custom_failure{},custom_initializations{},custom_updates{},custom_shutdowns{};
const char* custom_label="=sample_controller";
std::vector<unsigned char> custom_program;
bool vm_live() noexcept {return custom_vm_live;}
bool owner_live() noexcept {return custom_owner_live;}
constexpr uint64_t owner_id=0x200000001;
Owner owner() {return {reinterpret_cast<uintptr_t>(world.data()),owner_id};}
template<class T> void put(void* p,size_t offset,T value) {std::memcpy(static_cast<unsigned char*>(p)+offset,&value,sizeof(value));}
template<class T> T get(void* p,size_t offset) {T v;std::memcpy(&v,static_cast<unsigned char*>(p)+offset,sizeof(v));return v;}
void require(bool value,const char* text) {if(!value) {std::cerr<<text<<'\n';std::exit(1);}}
int lifecycle_return(int status,int phase) {
    if(lifecycle_phase!=phase) return status;
    lifecycle_phase=0;
    if(custom_mode) {
        custom_owner_live=false;
        if(lifecycle_kind==2) {custom_vm_live=false;closed_vm=true;registry.clear();vm={};}
        return status;
    }
    const auto id=reinterpret_cast<uintptr_t>(global.data());
    if(lifecycle_kind==2) {
        session::close_begin(id);registry.clear();session::close_end();closed_vm=true;
        // Also cover a closed address whose frame remains readable: pointer
        // equality must not permit restoring a stack belonging to a reused VM.
        if(phase!=4) vm={};
    } else {
        session::cleanup_begin(id,lifecycle_kind==3?owner_id+1:owner_id);session::cleanup_end();
    }
    return status;
}
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
    if(custom_mode) {
        const auto expected=custom_program.empty()?std::span<const unsigned char>(custom_bytes):std::span<const unsigned char>(custom_program);
        require(l==vm.data() && std::strcmp(label,custom_label)==0 &&
            size==expected.size() && !std::memcmp(bytes,expected.data(),size),"executor passes caller program unchanged");
        const auto id=get<uint64_t>(cell(env).data(),0);
        const auto meta=get<uint64_t>(tables[id]["__test_meta"].data(),0);
        require(get<uint64_t>(tables[id]["self"].data(),0)==owner_id &&
            get<uint64_t>(tables[id]["_ENV"].data(),0)==id &&
            get<uint64_t>(tables[meta]["__index"].data(),0)==1,"custom controller receives private owner environment");
        ++loads;
        if(custom_failure==1) {push_error("sample_controller:7: malformed chunk");return 1;}
        custom_loading=true;push(7);return 0;
    }
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
    if(custom_mode) {
        require(l==vm.data() && err==0,"custom call uses existing error barrier");
        const bool constructing=custom_loading;
        require(results==(constructing?1:0) && args==(constructing?0:args?1:0),"ordinary callbacks need no numeric return");
        if(args) require(cell(-1)==cell(-2),"shutdown receives a rooted truthy command");
        put(vm.data(),8,get<uintptr_t>(vm.data(),8)-(args+1)*24);
        if(constructing) {
            custom_loading=false;++custom_initializations;
            if(custom_failure==2) {push_error("sample_controller:9: assertion failed");return 2;}
            push(custom_failure==3?0:7);return lifecycle_return(0,1);
        }
        if(args) ++custom_shutdowns;else ++custom_updates;
        if(custom_failure==(args?5:4)) {push_error("sample_controller:12: attempt to call a nil value");return 2;}
        return lifecycle_return(0,args?3:2);
    }
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
        if(persistent_loading) {persistent_loading=false;push(7);return lifecycle_return(0,1);}
        if(args) {++listener_shutdowns;push(3,mode==16?-401:persistent_counter);return lifecycle_return(0,3);}
        ++persistent_counter;
        if(persistent_rollback && persistent_counter==1) {push(3,-410);return 0;}
        if(persistent_error && persistent_counter==3) {
            const auto message=std::string(persistent_event_mode?"crml_persistent_events:":"crml_persistent_error:")+
                std::to_string(persistent_event_mode?bytecode::persistent_events_error_line:bytecode::persistent_error_error_line)+": attempt to call a nil value";
            push_error(message.c_str());return 2;
        }
        push(3,persistent_counter);return lifecycle_return(0,2);
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
    require(!closed_vm,"provider must not restore a closed VM stack");
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
    require(!closed_vm,"provider must not retain in a closed VM");
    require(index==-1 && get<int>(cell(index).data(),16)==7,"retain returned callback");
    if(mode==13) throw 4;
    registry[++retains]=cell(index);lifecycle_return(0,4);return retains;
}
int fetch(void*,int index,int reference) {
    require(!closed_vm,"provider must not fetch from a closed VM");
    require(index==-10000 && registry.contains(reference),"fetch only owned live registry reference");
    ++fetches;
    if(mode==15) {push(3);return 3;}
    push_cell(registry.at(reference));return 7;
}
void release(void*,int reference) {
    require(!closed_vm,"provider must not release into a closed VM");
    require(registry.contains(reference),"reference released exactly once");
    ++releases;registry.erase(reference);
    lifecycle_return(0,5);
    if(mode==14) throw 4; // Even an ambiguous release may have recycled the slot.
}
Api fake_api() {return {&load,&protect,&settop,&call,&push_entity,&new_table,&push_value,&set_field,&readonly,&set_metatable,&raw_field,&retain,&fetch,&release};}
void reset(int m=0) {
    vm={};context={};global={};stack={};moved={};ci={};world={};generations={0,2};
    calls=loads=protects=restores=0;mode=m;deliberate=false;
    in_body=false;tables.clear();next_table=1;
    persistent_loading=persistent_error=false;persistent_environment=0;persistent_counter=retains=fetches=releases=0;registry.clear();
    persistent_event_mode=persistent_rollback=false;listener_shutdowns=listener_error_initializations=rollback_initializations=0;
    lifecycle_phase=lifecycle_kind=0;closed_vm=false;
    custom_mode=custom_loading=false;custom_vm_live=custom_owner_live=true;
    custom_failure=custom_initializations=custom_updates=custom_shutdowns=0;
    custom_label="=sample_controller";
    custom_program.clear();
    auto ptr=[](auto& a) {return reinterpret_cast<uintptr_t>(a.data());};
    put(vm.data(),8,ptr(stack)+48);put(vm.data(),0x10,ptr(stack)+24);
    put(vm.data(),0x18,ptr(global));put(vm.data(),0x20,ptr(ci));
    put(vm.data(),0x28,ptr(stack)+stack.size()-24);put(vm.data(),0x30,ptr(stack));
    put(vm.data(),0x40,ptr(ci));put(vm.data(),0x58,uintptr_t{0x1234});
    put(vm.data(),0x78,ptr(context));put(context.data(),0,ptr(world));
    put(world.data(),0x584e8,ptr(generations));put(world.data(),0x58510,uint64_t{2});
    put(ci.data(),0,ptr(stack)+24);put(stack.data(),24+16,uint32_t{7});
    configure(fake_api(),image);
}
std::string output() {std::ostringstream out;write(out);return out.str();}
void invoke() {after_call(vm.data(),site,1,0,1,0,owner());}
native::Execution custom_execute(native::Action action,int reference=0) {
    const native::Context current{vm.data(),reinterpret_cast<uintptr_t>(global.data()),owner().world,owner_id};
    return native::execute(fake_api(),current,action,reference,{custom_bytes,"=sample_controller"},{},{&vm_live,&owner_live});
}
void custom_checks() {
    reset();custom_mode=true;
    const auto initial=custom_execute(native::Action::initialize);
    require(initial.attempted && initial.restored && !initial.status && initial.reference>0 &&
        custom_initializations==1 && custom_updates==0,"standalone controller retained before first update");
    const auto update=custom_execute(native::Action::invoke,initial.reference);
    require(update.attempted && update.restored && !update.status && custom_updates==1,"ordinary nil-return controller invoked without probe stages");
    const auto unload=custom_execute(native::Action::unload,initial.reference);
    require(unload.restored && !unload.status && unload.shutdown && unload.released && unload.release_attempted &&
        custom_shutdowns==1 && registry.empty(),"nil-return shutdown releases the separate host root");
    for(int failure:{1,2,3}) {
        reset();custom_mode=true;custom_failure=failure;
        const auto result=custom_execute(native::Action::initialize);
        require(result.attempted && result.restored && result.status && !result.reference && registry.empty(),"failed constructor publishes no reference");
        if(failure!=3) require(result.error.line==(failure==1?7u:9u),"constructor error classified using caller label");
    }
    for(int failure:{4,5}) {
        reset();custom_mode=true;const auto ref=custom_execute(native::Action::initialize).reference;custom_failure=failure;
        const auto result=custom_execute(failure==4?native::Action::invoke:native::Action::unload,ref);
        require(result.restored && result.status==2 && result.error.line==12 &&
            std::strcmp(result.error.kind,"nil_call")==0,"custom callback error bounded and attributed before restoration");
        if(failure==4) {
            require(registry.size()==1 && releases==0,"executor leaves failed callback retirement to lifecycle owner");
            const auto released=custom_execute(native::Action::release,ref);
            require(released.released && released.restored && !custom_shutdowns,"raw retirement does not call stale Lua shutdown");
        } else require(result.released && !result.shutdown && releases==1 && registry.empty(),"failed shutdown releases its root without claiming shutdown success");
    }
    for(int phase:{1,2,3,4}) for(int kind:{1,2}) {
        reset();custom_mode=true;
        int ref=0;
        if(phase==2 || phase==3) ref=custom_execute(native::Action::initialize).reference;
        lifecycle_phase=phase;lifecycle_kind=kind;
        const auto prior_restores=restores;
        const auto result=custom_execute(phase==2?native::Action::invoke:phase==3?native::Action::unload:native::Action::initialize,ref);
        require(result.attempted && !result.release_attempted && releases==0,"custom interruption never runs another API operation");
        if(kind==2) require(!result.restored && restores==prior_restores && registry.empty(),"custom closed VM is never restored");
        else require(result.restored && restores==prior_restores+1,"custom retired owner can restore its still-live VM frame");
        if(phase==1) require(retains==0,"custom retired constructor is not retained");
    }
    reset();custom_mode=true;
    const native::Context ctx{vm.data(),reinterpret_cast<uintptr_t>(global.data()),owner().world,owner_id};
    const native::Program program{custom_bytes,"=sample_controller"};
    const native::Liveness live{&vm_live,&owner_live};
    const auto reject=[&](Api api,native::Action action,int ref,native::Program code,native::Options options,native::Liveness guard) {
        const auto result=native::execute(api,ctx,action,ref,code,options,guard);
        require(!result.attempted && result.status==-309 && protects==0 && loads==0 && registry.empty(),"incomplete executor request rejected before any native call");
    };
    auto incomplete=fake_api();incomplete.call=nullptr;
    reject(incomplete,native::Action::initialize,0,program,{},live);
    reject(fake_api(),native::Action::initialize,0,program,{},{});
    reject(fake_api(),native::Action::initialize,0,{custom_bytes,"=private/path.lua"},{},live);
    reject(fake_api(),native::Action::initialize,0,{{},"=empty"},{},live);
    reject(fake_api(),native::Action::initialize,0,program,{native::Returns::discard,4},live);
    reject(fake_api(),native::Action::initialize,0,program,{static_cast<native::Returns>(9)},live);
    reject(fake_api(),native::Action::invoke,0,program,{},live);
    reject(fake_api(),native::Action::release,-1,{}, {},live);
    const auto snapshot=stack;generations[1]=3;
    require(!custom_execute(native::Action::initialize).attempted && stack==snapshot && !protects,"executor rejects replaced owner without stack changes");
    reset();custom_mode=true;custom_vm_live=false;
    require(!custom_execute(native::Action::initialize).attempted && !protects,"executor rejects invalidated VM before capture");

    // End-to-end native path: the real host schedules the real executor against
    // the same fake engine ABI used by the diagnostic provider tests.
    reset();custom_mode=true;custom_label="=crml_sample";
    native::ControllerHost host(fake_api());
    require(host.submit("sample",{std::begin(custom_bytes),std::end(custom_bytes)}),"host queues caller-owned compiled program");
    const auto current_context=[] {return native::Context{vm.data(),reinterpret_cast<uintptr_t>(global.data()),owner().world,owner_id,native::dispatch::revision()};};
    host.tick(current_context(),0);host.tick(current_context(),16);
    require(host.snapshot().initialized==1 && host.snapshot().invoked==1 && registry.size()==1,"host and executor integrate without diagnostic counter returns");
    native::dispatch::cleanup_begin(reinterpret_cast<uintptr_t>(global.data()),owner_id);native::dispatch::teardown_end();
    host.tick(current_context(),17);
    require(registry.empty() && releases==1 && custom_shutdowns==0,"actual executor raw release follows host owner retirement");
    host.tick(current_context(),18);host.stop();host.tick(current_context(),19);host.tick(current_context(),20);
    require(host.snapshot().packages==0 && custom_shutdowns==1 && registry.empty(),"reinitialized controller shuts down normally through native provider");
}
#ifdef CRML_LUA_SOURCE
void source_hook_checks() {
    namespace fs=std::filesystem;
    namespace source=native::source;
    reset();custom_mode=true;custom_label="=crml_sample";
    const std::string code="local count=0\nreturn function(stop)\n if stop then return end\n count+=1\nend\n";
    custom_program=native::compile_source(code).bytes;require(!custom_program.empty(),"source fixture compiled");
    const auto root=fs::absolute("lua-source-hook-test-"+std::to_string(GetCurrentProcessId()));
    require(fs::create_directory(root),"exclusive source hook fixture");
    fs::create_directories(root/"lua-mods/sample");
    {std::ofstream file(root/"lua-mods/sample/main.luau");file<<code;}
    require(!source::prepare(root) && !source::active(),"source build alone does not opt in");
    {std::ofstream marker(root/"engine-lua.enabled");}
    require(source::prepare(root) && !source::active(),"source preparation alone cannot execute code");
    require(source::attach(fake_api()),"authenticated provider attaches source host");
    source::poll(0);
    const auto update=[] {
        push(7);push_entity(vm.data(),owner_id,1);
        const auto captured=before_call(vm.data(),site,1,0,1);
        require(captured.entity==owner_id,"source hook captures current owner");
        settop(vm.data(),1); // Simulate the successful original engine call.
        after_call(vm.data(),site,1,0,1,0,captured);
    };
    after_call(vm.data(),site+1,1,0,1,0,owner());require(loads==0,"source mode still rejects other call sites");
    update();require(loads==1 && custom_initializations==1 && registry.size()==1,"source initializes without running diagnostic smoke stages");
    Sleep(20);update();require(custom_updates==1,"source controller executes on later authenticated update");
    stop();Sleep(20);update();require(custom_updates==2,"ending diagnostic recording cannot stop source mods");
    require(output().find("\"mode\":\"source\"")!=std::string::npos,"source mode is distinct from diagnostic success");
    fs::remove(root/"engine-lua.enabled");source::poll(500);update();update();
    require(registry.empty() && custom_shutdowns==1 && source::active(),"opt-out drains callbacks and keeps watcher available");
    {std::ofstream marker(root/"engine-lua.enabled");}
    source::poll(1000);update();Sleep(20);update();
    require(custom_initializations==2 && custom_updates==3 && registry.size()==1,"opt-in can load source again after drain");
    source::stop();update();update();source::poll(1500);
    require(!source::active() && registry.empty() && custom_shutdowns==2,"permanent service stop drains and closes its worker log");
    std::ifstream file(root/"lua-mods.jsonl");const std::string log{std::istreambuf_iterator<char>(file),{}};file.close();
    require(log.find("\"initialized\":2")!=std::string::npos && log.find("\"released\":2")!=std::string::npos &&
        log.find("lua_source_stopped")!=std::string::npos,"source log records actual lifecycle totals");
    require(log.find(root.string())==std::string::npos && log.find(code)==std::string::npos,"source log contains no root path or source text");
    fs::remove_all(root); // Exclusively created fixture, log closed after drain.
}
#endif
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
    for(int phase:{1,2,3,4}) for(int kind:{1,2}) {
        reset();invoke();configure_persistent(true);session_context.revision=session::revision();
        if(phase==2 || phase==3) session::tick(session_context,0);
        if(phase==3) session::stop();
        lifecycle_phase=phase;lifecycle_kind=kind;
        const auto before_restore=restores;
        session::tick(session_context,250);
        std::ostringstream interrupted;session::write(interrupted);
        require(interrupted.str().find("\"interrupted_calls\":1")!=std::string::npos,"provider teardown interrupts its operation");
        if(kind==2) {
            require(closed_vm && restores==before_restore && releases==0 && registry.empty(),"provider performs no stack restoration or unref after synchronous VM close");
            require(interrupted.str().find("\"active_reference\":false")!=std::string::npos,"provider never publishes a root from a closed VM");
        } else {
            require(restores==before_restore+1,"live VM stack restored after owner cleanup");
            require(releases==0,"owner interruption defers root release to a fresh frame");
            if(phase==1) require(retains==0 && registry.empty(),"constructor returning after cleanup must not retain its closure");
            else {
                session_context.revision=session::revision();session::tick(session_context,501);
                require(releases==1 && registry.empty(),"interrupted operation drains its separate root once");
                if(phase==3) require(listener_shutdowns==1,"interrupted script shutdown never repeats");
            }
        }
    }
    reset();invoke();configure_persistent(true);session_context.revision=session::revision();session::tick(session_context,0);
    lifecycle_phase=2;lifecycle_kind=3;session::tick(session_context,250);
    require(persistent_counter==1 && restores==7,"unrelated owner cleanup leaves current invocation valid");
    reset();invoke();configure_persistent(true);session_context.revision=session::revision();session::tick(session_context,0);session::stop();
    lifecycle_phase=5;lifecycle_kind=1;mode=14;session::tick(session_context,250);
    session_context.revision=session::revision();session::tick(session_context,500);
    require(releases==1 && registry.empty(),"reentrant teardown plus ambiguous unref cannot release a recycled slot twice");
    std::ostringstream ambiguous;session::write(ambiguous);
    require(ambiguous.str().find("\"failures\":1")!=std::string::npos,"ambiguous interrupted release is reported as failure");
    custom_checks();
#ifdef CRML_LUA_SOURCE
    source_hook_checks();
#endif
    std::cout<<"Lua probe and executor checks passed\n";
}
