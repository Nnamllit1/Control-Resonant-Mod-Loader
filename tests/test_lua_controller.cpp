#include "lua_controller.h"
#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace crml::engine::lua;
namespace {
struct Call {Context context;Action action;int reference;unsigned source;};
struct Root {uintptr_t global;uint64_t owner;unsigned source;};
std::vector<Call> calls;
std::map<int,Root> roots;
int serial{},failure{},reentry{};
Action special=Action::invoke;
ControllerHost* host{};
HANDLE entered{},resume{};
void require(bool ok,const char* message) {if(!ok) {std::cerr<<message<<'\n';std::exit(1);}}
Context context(uint64_t owner=30,uintptr_t global=10,uintptr_t world=20) {
    return {reinterpret_cast<void*>(1),global,world,owner,dispatch::revision()};
}
std::vector<unsigned char> code(unsigned char marker=1) {return {6,3,marker};}
Execution provider(const Api&,Context ctx,Action action,int reference,Program program,Options options,Liveness live) {
    calls.push_back({ctx,action,reference,program.bytes[2]});
    require(options.returns==Returns::discard && !options.environment_arguments && !options.global_must_be_nil,"host invokes ordinary controller contract");
    require(live.owner_live() && live.vm_live(),"new operation is live under the shared gate");
    require(program.label && std::string_view(program.label).starts_with("=crml_"),"package label has no filesystem path");
    if(action==special && failure==4) return {};
    if(action!=Action::initialize) {
        require(roots.contains(reference) && roots.at(reference).global==ctx.global,"reference never crosses VM or reuses retired slot");
        if(action!=Action::release) require(roots.at(reference).owner==ctx.owner,"Lua callbacks keep their original owner");
        require(roots.at(reference).source==program.bytes[2],"old controller retains old source during reload");
    }
    if(entered && action==Action::invoke) {SetEvent(entered);require(WaitForSingleObject(resume,5000)==WAIT_OBJECT_0,"resume invocation");}
    if(action==special && failure==5) throw std::runtime_error("native failure");
    Execution result;result.attempted=result.restored=true;
    if(action==Action::initialize && !(action==special && failure==1)) {
        result.reference=++serial;roots[serial]={ctx.global,ctx.owner,program.bytes[2]};
    }
    if(action==special && reentry) {
        const int event=reentry;reentry=0;
        const auto count=calls.size();host->tick(context(),99999);
        require(calls.size()==count,"nested host tick is skipped");
        (void)host->snapshot(); // Reentrant bookkeeping must not self-deadlock.
        if(event==1 || event==2 || event==3) {
            dispatch::cleanup_begin(event==3?0:ctx.global,event==2?ctx.owner+1:ctx.owner);dispatch::teardown_end();
        } else if(event==4) {
            dispatch::close_begin(ctx.global);
            for(auto it=roots.begin();it!=roots.end();) {if(it->second.global==ctx.global) it=roots.erase(it);else ++it;}
            dispatch::teardown_end();
        } else if(event==5) require(host->submit("sample",code(2)),"queue replacement during constructor");
        if(!live.owner_live()) return result;
    }
    if(action==Action::unload || action==Action::release) {
        result.release_attempted=true;roots.erase(reference);result.released=true;
        result.shutdown=action==Action::unload;
    }
    if(action==special) {
        if(failure==1) {result.status=2;result.shutdown=false;result.error.line=42;}
        if(failure==2) result.restored=false;
        if(failure==3) {result.status=4;result.released=false;}
    }
    return result;
}
void reset() {require(roots.empty(),"previous test left no live roots");dispatch::reset_for_test();calls.clear();failure=reentry=0;special=Action::invoke;entered=resume=nullptr;}
void tick(uint64_t time=0,uint64_t owner=30,uintptr_t global=10,uintptr_t world=20) {host->tick(context(owner,global,world),time);}
void drain(uint64_t owner=30) {failure=reentry=0;host->stop();tick(100000,owner);tick(100001,owner);require(roots.empty() && host->snapshot().references==0,"stop drains references");}
void close_all() {dispatch::close_begin(10);roots.clear();dispatch::teardown_end();}
}
int main() {
    {
        reset();ControllerHost manager({},&provider);host=&manager;
        require(manager.submit("sample",code()) && manager.snapshot().pending==1,"compile result queued without native calls");
        require(calls.empty(),"submission stays off VM");tick();require(roots.size()==1,"initialization retains controller");
        tick(15);require(calls.size()==1,"update interval honored");
        tick(16,31);tick(16,30,11);require(calls.size()==1,"foreign owner and VM cannot run callback");
        tick(16);require(manager.snapshot().invoked==1,"ordinary controller update");
        manager.submit("sample",code(2));tick(17);
        require(calls.back().action==Action::unload && calls.back().source==1 && roots.empty(),"reload unloads old revision first");
        tick(18);require(calls.back().action==Action::initialize && calls.back().source==2,"replacement initializes after drain");
        drain();require(manager.snapshot().packages==0,"unloaded slot reusable");
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;
        manager.submit("sample",code());special=Action::initialize;reentry=5;tick();
        require(calls.back().source==1 && roots.size()==1,"reentrant replacement does not alter in-flight source");
        tick(1);require(calls.back().action==Action::unload && calls.back().source==1,"reentrant replacement cleans old controller");
        tick(2);require(calls.back().action==Action::initialize && calls.back().source==2,"reentrant replacement later loads newest source");drain();
    }
    for(const auto action:{Action::initialize,Action::invoke,Action::unload}) for(int event:{1,2,4}) {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());
        if(action!=Action::initialize) tick();
        if(action==Action::unload) manager.unload("sample");
        special=action;reentry=event;tick(20);
        const auto after=manager.snapshot();
        if(event==4) {
            require(after.references==0 && after.reclaimed==1 && roots.empty(),"synchronous VM close cannot publish or reuse controller root");
        } else if(event==1) {
            require(after.references==1 && after.interrupted==1,"retired root waits for eligible context");
            tick(21,31);require(calls.back().action==Action::release && roots.empty(),"engine retirement performs raw release on fresh live owner");
        } else require(after.interrupted==0,"foreign cleanup preserves controller");
        drain(event==1?31:30);
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;
        manager.submit("sample",code());manager.submit("other",code(2));special=Action::initialize;reentry=1;
        tick();require(calls.size()==1,"teardown invalidates context before second package");drain();
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();
        const auto stale=context();dispatch::cleanup_begin(10,30);manager.tick(context(),100);dispatch::teardown_end();
        manager.tick(stale,100);require(calls.size()==1,"teardown depth and stale revisions reject execution");
        tick(101,31);require(calls.back().action==Action::release,"retired reference drains after teardown");drain(31);
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();
        tick(100,31,10,21);require(calls.back().action==Action::release,"world replacement retires without stale shutdown");
        tick(101,31,10,21);require(calls.back().action==Action::initialize,"controller initializes in replacement world");
        manager.stop();tick(200,31,10,21);tick(201,31,10,21);require(roots.empty(),"replacement world drains");
    }
    for(const auto action:{Action::initialize,Action::invoke,Action::unload}) {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());
        if(action!=Action::initialize) tick();
        if(action==Action::unload) manager.submit("sample",code(2));
        special=action;failure=1;tick(20);failure=0;tick(21);tick(40);
        require(manager.snapshot().failures>0,"controller failure reported");
        require(manager.packages().front().status==2 && manager.packages().front().line==42,
            "successful cleanup preserves the original package failure and source line");
        if(action==Action::unload) {
            require(manager.snapshot().awaiting_owner==1 && roots.empty(),"failed shutdown blocks replacement despite released root");
            const auto count=calls.size();tick(100);require(calls.size()==count,"failed shutdown never retried or silently reinitialized");
            dispatch::cleanup_begin(10,30);dispatch::teardown_end();tick(101,31);
            require(calls.back().action==Action::initialize && calls.back().source==2,"queued replacement allowed after engine owner cleanup");drain(31);
        } else {
            require(roots.empty(),"failed controller drained");
            const auto count=calls.size();tick(100);require(calls.size()==count,"failed revision does not loop retry");
            require(manager.submit("sample",code(2)),"explicit new revision can recover");tick(101);drain();
        }
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();manager.unload("sample");
        special=Action::unload;failure=3;tick(20);failure=0;const auto count=calls.size();tick(21);
        require(manager.snapshot().ambiguous_releases==1 && roots.empty() && calls.size()==count,"ambiguous unref is not retried");
        close_all();drain();
    }
    for(int bad:{2,5}) {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();failure=bad;
        bool thrown=false;try {tick(20);} catch(const std::runtime_error&) {thrown=true;}
        require(thrown==(bad==5) && manager.snapshot().halted,"unsafe frame or native exception halts host");
        const auto count=calls.size();tick(21);require(calls.size()==count,"halted host cannot touch uncertain VM again");close_all();
        ControllerHost second({},&provider);
        require(!second.submit("another",code()) && second.snapshot().halted,"unsafe native outcome also stops other hosts");
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();reentry=3;tick(20);
        require(manager.snapshot().halted,"unknown teardown identity halts host");close_all();
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;
        require(!manager.submit("../private",code()) && !manager.submit("bad",{6,2,1}) && !manager.submit("bad",code(),0),"invalid package requests rejected");
        for(unsigned i=0;i<ControllerHost::capacity;++i) require(manager.submit("p"+std::to_string(i),code()),"bounded capacity admits package");
        require(!manager.submit("overflow",code()),"capacity never grows on engine thread");
        tick();require(roots.size()==ControllerHost::capacity,"independent controller roots");drain();
    }
    {
        reset();ControllerHost manager({},&provider);host=&manager;manager.submit("sample",code());tick();
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        require(entered && resume,"create synchronization events");
        std::atomic<bool> cleaned{};
        std::thread invoke([]{tick(20);});require(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"invocation entered");
        std::thread cleanup([&]{dispatch::cleanup_begin(10,30);cleaned=true;dispatch::teardown_end();});
        Sleep(20);require(!cleaned.load(),"other-thread cleanup waits for callback");SetEvent(resume);invoke.join();cleanup.join();
        CloseHandle(entered);CloseHandle(resume);entered=resume=nullptr;
        tick(21,31);require(calls.back().action==Action::release,"serialized cleanup retires callback after return");drain(31);
    }
    require(!dispatch::configured(),"destroyed hosts disconnect lifecycle observers");
    std::cout<<"Lua controller host checks passed\n";
}
