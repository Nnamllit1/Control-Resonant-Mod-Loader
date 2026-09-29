#include "lua_session.h"
#include <Windows.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace crml::probe::lua::session;
namespace {
struct Call {Context context;Action action;int reference;};
std::vector<Call> calls;
int next_ref{},counter{},mode{};
bool deliberate{};
HANDLE entered{},resume{};
Context context{reinterpret_cast<void*>(1),0xabc123456,20,30};
void require(bool ok,const char* message) {if(!ok) {std::cerr<<message<<'\n';std::exit(1);}}
Result execute(Context c,Action action,int reference) {
    calls.push_back({c,action,reference});
    if(mode==1) return {}; // Unsuitable frame must not consume ownership.
    if(action==Action::initialize || action==Action::initialize_error) {
        counter=0;deliberate=action==Action::initialize_error;
        return {true,true,false,++next_ref,mode==2?4:0,0};
    }
    if(action==Action::release || action==Action::unload) return {true,true,mode!=3,0,mode==3?4:0,double(counter),action==Action::unload};
    if(entered) {
        SetEvent(entered);
        require(WaitForSingleObject(resume,5000)==WAIT_OBJECT_0,"resume blocked operation");
    }
    ++counter;
    return {true,mode!=4,false,0,deliberate && counter==3?2:0,double(counter),false,mode!=5 && deliberate && counter==3};
}
void reset() {calls.clear();next_ref=counter=mode=0;deliberate=false;entered=resume=nullptr;reset_for_test(execute);}
std::string report() {std::ostringstream out;write(out);return out.str();}
void field(const char* name,int value) {
    require(report().find(std::string("\"")+name+"\":"+std::to_string(value)+",")!=std::string::npos,name);
}
void current_tick(Context c,uint64_t now) {c.revision=revision();tick(c,now);}
void stage_three() {for(uint64_t time=0;time<=2500;time+=250) tick(context,time);}
}
int main() {
    reset();tick(context,0);tick(context,249);require(calls.size()==1,"callback waits for later timed update");
    tick(context,250);require(calls.back().action==Action::invoke && calls.back().reference==1,"retained callback invoked");
    tick(context,500);tick(context,750);tick(context,751);
    field("explicit_unloads",1);field("released",1);field("invocations",3);
    require(calls.back().action==Action::release,"explicit unload releases reference");
    reset();stage_three();field("initialized",3);field("expected_errors",1);field("released",2);field("failures",0);
    const auto before=calls.size();cleanup_begin(context.global,context.owner);
    tick(context,3000);require(calls.size()==before,"no execution inside engine cleanup");
    cleanup_end();field("owner_retirements",1);
    tick(context,3000);require(calls.size()==before,"pre-cleanup call token cannot access a still-valid entity after teardown");
    auto new_owner=context;new_owner.owner=31;
    current_tick(new_owner,3000);require(calls.back().action==Action::release && calls.back().reference==3,"retired ref released on later eligible update");
    current_tick(new_owner,3001);field("initialized",4);require(calls.back().action==Action::initialize,"replacement owner initializes fresh closure");
    cleanup_begin(context.global,context.owner);cleanup_end();field("owner_retirements",1);
    close_begin(context.global);field("vm_reclaimed",1);
    const auto closed_calls=calls.size();tick(new_owner,4000);require(calls.size()==closed_calls,"no execution while VM closes");
    close_end();current_tick(new_owner,4001);require(calls.back().action==Action::initialize,"same global address after close does not reuse ref");
    stop();current_tick(new_owner,4002);field("released",4);
    const auto stopped=calls.size();tick(new_owner,5000);require(calls.size()==stopped && !needs_calls(),"unloaded session never restarts");

    reset();tick(context,0);auto foreign=context;foreign.global++;
    tick(foreign,250);require(calls.size()==1,"never fetch reference from another VM");
    auto world=context;world.world++;
    tick(world,250);field("world_retirements",1);field("released",1);
    reset();tick(context,0);mode=1;stop();tick(context,250);require(needs_calls(),"failed frame capture retains pending cleanup");
    mode=0;tick(context,500);field("released",1);
    reset();mode=2;tick(context,0);mode=0;tick(context,250);field("failures",1);field("released",1);
    require(!needs_calls(),"partially initialized ref released without retrying script");
    reset();tick(context,0);mode=3;stop();tick(context,250);const auto ambiguous=calls.size();tick(context,500);
    require(calls.size()==ambiguous,"ambiguous unref is never retried");field("failures",1);
    reset();tick(context,0);mode=4;tick(context,250);const auto corrupt=calls.size();tick(context,500);
    require(calls.size()==corrupt && report().find("\"halted\":true")!=std::string::npos,"stack damage stops further VM access");
    reset();tick(context,0);cleanup_begin(0,30);cleanup_end();const auto unreadable=calls.size();tick(context,250);
    require(calls.size()==unreadable,"unreadable teardown identity halts reference use");

    reset();tick(context,0);
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE retiring=CreateEventW(nullptr,TRUE,FALSE,nullptr),retired=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    require(entered && resume && retiring && retired,"create thread barriers");
    std::thread callback([]{tick(context,250);});
    require(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"callback entered");
    std::thread cleanup([&]{SetEvent(retiring);cleanup_begin(context.global,context.owner);SetEvent(retired);});
    require(WaitForSingleObject(retiring,5000)==WAIT_OBJECT_0,"cleanup requested");
    require(WaitForSingleObject(retired,20)==WAIT_TIMEOUT,"cleanup waits for in-flight protected callback");
    SetEvent(resume);callback.join();cleanup.join();
    const auto draining=calls.size();tick(context,500);require(calls.size()==draining,"blocked until original cleanup finishes");
    cleanup_end();current_tick(context,501);field("owner_retirements",1);field("released",1);
    for(auto handle:{entered,resume,retiring,retired}) CloseHandle(handle);
    require(report().find(std::to_string(context.global))==std::string::npos,"no raw global identity serialized");
    reset();reset_for_test(execute,true);stage_three();
    require(calls[4].action==Action::unload && calls[9].action==Action::unload,"live-owner explicit and error cleanup invoke Lua shutdown");
    cleanup_begin(context.global,context.owner);cleanup_end();current_tick(context,3000);
    require(calls.back().action==Action::release,"engine owner cleanup skips Lua shutdown");
    reset();reset_for_test(execute,true);tick(context,0);stop();
    auto other_owner=context;other_owner.owner++;
    tick(other_owner,250);require(calls.size()==1,"live shutdown waits for original owner context");
    cleanup_begin(context.global,context.owner);cleanup_end();current_tick(other_owner,251);
    require(calls.back().action==Action::release,"owner teardown supersedes pending explicit unload");
    reset();reset_for_test(execute,true);mode=5;
    for(uint64_t time=0;time<=2000;time+=250) tick(context,time);
    field("expected_errors",0);field("failures",1);
    std::cout<<"Lua session checks passed\n";
}
