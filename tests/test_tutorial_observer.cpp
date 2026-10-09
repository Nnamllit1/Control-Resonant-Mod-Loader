#include "diagnostics/tutorial_observer.h"
#include "compatibility.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace crml::tutorial_observer {bool test_supported() noexcept;bool test_callers() noexcept;bool test_callthrough();bool test_sampling() noexcept;bool test_reporting();bool test_native_unwind();}
using namespace crml::tutorial_observer;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main() {
    try {
        Admissions gate;
        require(!gate.enter() && !gate.pending(),"observation admission starts closed");
        gate.open();
        std::atomic<bool> ready{},resume{};
        bool admitted{};
        // Represents a worker paused after reading the capture deadline, but
        // before admission. Closing must reject its later attempt to enter.
        std::thread delayed([&] {
            ready.store(true);ready.notify_one();
            resume.wait(false);
            admitted=gate.enter();
            if(admitted) gate.leave();
        });
        ready.wait(false);gate.close();resume.store(true);resume.notify_one();delayed.join();
        require(!admitted && !gate.pending(),"stale deadline cannot admit a span after closure");
        gate.open();ready.store(false);resume.store(false);
        Recorder delayed_recorder;
        // Admission must cover the gap before Recorder::enter, not just calls
        // already visible in per-phase in_flight counts.
        std::thread counted_later([&] {
            admitted=gate.enter();ready.store(true);ready.notify_one();resume.wait(false);
            if(admitted) {
                auto e=delayed_recorder.enter(Phase::requests,1,100);
                e.returned=true;delayed_recorder.leave(e);gate.leave();
            }
        });
        ready.wait(false);gate.close();
        const bool pending_before_count=gate.pending()==1 && !delayed_recorder.counts(Phase::requests).calls && !gate.enter();
        resume.store(true);resume.notify_one();counted_later.join();
        Event delayed_event{};
        require(pending_before_count && !gate.pending() && delayed_recorder.drain(&delayed_event,1)==1,
            "closed admission remains pending until the late span is recorded");
        uint64_t token=123,other{};
        uintptr_t two[]{0x1000,0x2000,0x3000,3};
        uintptr_t one[]{0x2000,0x3000,3};
        require(inspect(Phase::initialize,1,1,two,42,token)==Query::ok,"initializer aggregate");
        require(inspect(Phase::hint_sync,1,1,one,42,other)==Query::ok && token==other,"same data row correlates across different aggregates");
        require(inspect(Phase::panel_sync,1,1,one,43,other)==Query::ok && token!=other,"session salt changes token");
        two[0]=0x2000;
        require(inspect(Phase::panel_events,1,1,two,42,other)==Query::ok && token==other,"event aggregate layout");
        uintptr_t requests[]{0x9000,0x8000,0x7000,0x6000,0x2000,0x5000,0x4000,0x3000,3};
        require(inspect(Phase::requests,1,1,requests,42,other)==Query::ok && token==other,"request writer and UI reader correlate the same TutorialData row");
        require(requests[4]==0x2000 && requests[8]==3,"request aggregate is not changed");
        require(inspect(Phase::requests,2,1,reinterpret_cast<void*>(1),42,other)==Query::caller && !other,"foreign request caller rejected before reading aggregate");
        requests[8]=UINTPTR_MAX;
        require(inspect(Phase::requests,1,1,requests,42,other)==Query::range && !other,"request row overflow rejected");
        requests[8]=0;requests[4]=0;
        require(inspect(Phase::requests,1,1,requests,42,other)==Query::range && !other,"request missing data rejected");
        require(inspect(Phase::requests,1,1,reinterpret_cast<void*>(1),42,other)==Query::memory,"unreadable request aggregate rejected");
        const auto original=one[0];
        require(inspect(Phase::panel_sync,1,1,one,42,other)==Query::ok && one[0]==original,"read-only inspection");
        require(inspect(Phase::hint_sync,2,1,reinterpret_cast<void*>(1),42,other)==Query::caller && !other,"foreign caller never reads memory");
        require(inspect(Phase::hint_sync,1,1,reinterpret_cast<void*>(1),42,other)==Query::memory && !other,"unmapped query");
        require(inspect(Phase::hint_sync,1,1,nullptr,42,other)==Query::range && !other,"null query");
        one[2]=UINTPTR_MAX;
        require(inspect(Phase::hint_sync,1,1,one,42,other)==Query::range && !other,"stride overflow rejected");
        one[0]=0;one[2]=0;
        require(inspect(Phase::hint_sync,1,1,one,42,other)==Query::range && !other,"missing component rejected");
        Recorder r;
        auto a=r.enter(Phase::initialize,10,100);
        auto b=r.enter(Phase::hint_sync,20,101);
        require(!a.overlap && b.overlap==1,"reader entering during initialization records overlap");
        auto c=r.enter(Phase::initialize,30,102);
        require(c.overlap==3,"same-phase overlap also recorded");
        require(r.counts(Phase::initialize).in_flight==2,"unfinished calls remain explicit in reports");
        a.returned=true;a.end=110;r.leave(a);
        b.returned=true;b.end=120;r.leave(b);
        c.end=130;c.query=Query::caller;r.leave(c);
        Event output[256]{};
        require(r.drain(output,256)==3 && output[0].sequence==a.sequence && output[2].end==130,"paired spans preserve metadata");
        auto count=r.counts(Phase::initialize);
        require(count.calls==2 && count.overlaps==1 && count.unwinds==1 && count.rejected==1 && !count.in_flight,"counts preserve failure evidence");
        a=r.enter(Phase::initialize,10,200);
        require(!a.overlap,"unwind released active count");
        a.sequence=0;a.returned=true;r.leave(a);
        require(!r.drain(output,256),"unsampled calls counted without event storage");
        for(unsigned i=0;i<300;++i) {auto e=r.enter(Phase::panel_sync,10,i);e.returned=true;r.leave(e);}
        require(r.counts(Phase::panel_sync).dropped==44 && r.drain(output,256)==256,"queue overflow is bounded and explicit");
        require(!r.drain(output,256),"drain empties queue");
        auto writer=r.enter(Phase::requests,40,400);
        auto reader=r.enter(Phase::hint_sync,50,401);
        require(reader.overlap==(1u<<4),"request writer retains distinct phase4 overlap bit");
        reader.returned=true;r.leave(reader);writer.returned=true;r.leave(writer);
        require(r.drain(output,256)==2 && !r.counts(Phase::requests).in_flight,"request span completes without retaining active state");
        std::vector<std::thread> threads;
        for(unsigned i=0;i<4;++i) threads.emplace_back([&r,i] {
            for(unsigned j=0;j<1000;++j) {auto e=r.enter(Phase::panel_events,i,j);e.returned=true;r.leave(e);}
        });
        for(auto& thread:threads) thread.join();
        const auto stored=r.drain(output,256);count=r.counts(Phase::panel_events);
        require(count.calls==4000 && stored+count.dropped==4000,"concurrent contention never loses accounting or exceeds capacity");
        using namespace crml::compatibility;
        for(auto profile:{EngineProfile::previous,EngineProfile::october_update,EngineProfile::october_hotfix,EngineProfile::october_patch}) {
            engine_profile=profile;reviewed_build=false;
            require(!test_supported(),"unreviewed executable rejected");
            reviewed_build=true;
            require(test_supported()==(profile==EngineProfile::october_patch),"addresses restricted to mapped build");
        }
        require(test_callthrough(),"native wrappers preserve arguments, calls, exceptions and disabled behavior");
        require(test_callers(),"only reviewed callers admitted; foreign pointers not published as image offsets");
        require(test_sampling(),"sampling interval, exact ceiling and independent phase budgets");
        require(test_reporting(),"stop reports pending spans and later includes their completion before the final report");
        require(test_native_unwind(),"native structured exception releases observation admission and active span");
        std::cout<<"Tutorial observation tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
