#include "physics_session.h"
#include "physics_selection.h"
#include "physics_target_diagnostics.h"
#include "physics_lifetime_diagnostics.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <vector>
#include <memory>
#include <sstream>
using namespace crml::physics;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void test_lifetime_diagnostics() {
    LifetimeDiagnostics watch;LifetimeIdentity out{};
    require(watch.arm({1,2,3,0x100000004ull}),"arm identity watch");
    require(!watch.take(9,0x100000004ull,false,out),"different scene cannot retire target");
    require(!watch.take(2,0x200000004ull,false,out),"reused slot with different generation cannot retire target");
    require(watch.take(2,0x100000004ull,false,out) && out.selection==1 && out.entity==3,"matching full body identity retires once");
    require(!watch.take(2,0,true,out) && !out.selection,"no duplicate scene event after body retirement");
    require(watch.arm({2,2,5,6}) && watch.arm({3,7,8,9}),"new selection replaces diagnostic identity");
    require(!watch.take(2,6,false,out),"old selection no longer watched");
    require(watch.take(7,0,true,out) && out.selection==3 && out.body==9,"whole scene invalidates retained identity");
    require(watch.missed()==0,"uncontended diagnostics do not lose observations");
}
void test_target_diagnostics() {
    auto queue=std::make_unique<TargetDiagnostics>();
    TargetDiagnostic d{};d.search=1;d.player=10;d.entity=20;d.body=30;d.body_count=1;d.distance=.8f;
    d.components_valid=true;d.snapshot.count=2048;
    for(auto& c:d.snapshot.components) c.hash=UINT32_MAX;
    std::ostringstream text;write_target_diagnostic(text,d);
    require(text.str().size()<32768,"maximum component record fits parser bound");
    require(text.str().find("\"player\":\"10\"")!=std::string::npos && text.str().find("\"entity\":\"20\"")!=std::string::npos,"player and target remain distinct");
    for(size_t i=0;i<TargetDiagnostics::capacity;++i) {d.search=i+1;require(queue->push(d),"diagnostic queue accepts bounded records");}
    require(!queue->push(d) && queue->dropped()==1,"diagnostic overflow is counted");
    for(size_t i=0;i<TargetDiagnostics::capacity;++i) {require(queue->pop(d) && d.search==i+1,"diagnostic FIFO order");}
    require(!queue->pop(d),"empty diagnostic queue");
}
void test_selection() {
    SelectionSearch search;
    SelectionScope scope{1,2,3,4,16609,{0,0,0}};
    std::vector<unsigned> visits(scope.slots);
    auto visit=[&](uint32_t index,SelectionCandidate& candidate) {
        ++visits.at(index);
        if(index!=16608) return false;
        candidate={10,20,30};return true;
    };
    auto no_yield=[] {return false;};
    require(search.begin(scope,100)==SelectionResult::pending,"real scene count must be searchable");
    for(unsigned batch=0;batch<4;++batch) {
        require(search.step(scope,100+batch,visit,no_yield)==SelectionResult::pending,"do not select before complete scan");
        require(search.scanned()==(batch+1)*4096,"bounded slot work per callback");
    }
    require(search.step(scope,105,visit,no_yield)==SelectionResult::selected,"find prop beyond old table bound");
    require(search.candidate().body==20 && search.candidate().actor==30,"retain full identities");
    for(auto n:visits) require(n==1,"search each slot exactly once");
    search.begin(scope,100);
    require(search.step(scope,100,visit,[]{return true;})==SelectionResult::pending && search.scanned()==64,"elapsed work yield");
    for(unsigned change=0;change<6;++change) {
        search.begin(scope,100);auto other=scope;
        if(change==0) ++other.world;
        if(change==1) ++other.owner;
        if(change==2) ++other.player;
        if(change==3) ++other.retirement;
        if(change==4) ++other.slots;
        if(change==5) other.position[0]=.26f;
        require(search.step(other,101,visit,no_yield)==SelectionResult::changed && !search.scanned(),"scope change aborts without visiting");
    }
    for(auto now:{uint64_t(99),uint64_t(15100)}) {
        search.begin(scope,100);
        require(search.step(scope,now,visit,no_yield)==SelectionResult::timeout,"deadline and clock rollback");
    }
    search.begin(scope,100); search.cancel();
    require(search.step(scope,101,visit,no_yield)==SelectionResult::changed,"cancel prevents later selection");
    search.begin(scope,100);
    auto ambiguous=[](uint32_t i,SelectionCandidate& c) {c={10+i,20+i,30+i};return i==0 || i==4096;};
    require(search.step(scope,101,ambiguous,no_yield)==SelectionResult::pending,"first candidate does not end search");
    require(search.step(scope,102,ambiguous,no_yield)==SelectionResult::pending,"possible later nearest candidate must be considered");
    search.step(scope,103,ambiguous,no_yield);search.step(scope,104,ambiguous,no_yield);
    require(search.step(scope,105,ambiguous,no_yield)==SelectionResult::ambiguous,"equal nearest distances across batches");
    scope.slots=8193;
    for(unsigned nearest=0;nearest<3;++nearest) {
        search.begin(scope,100);
        auto crowded=[&](uint32_t i,SelectionCandidate& c) {
            if(i%4096) return false;
            c={10+i,20+i,30+i,i/4096==nearest?.25f:1.f};return true;
        };
        require(search.step(scope,101,crowded,no_yield)==SelectionResult::pending,"crowded first batch");
        require(search.step(scope,102,crowded,no_yield)==SelectionResult::pending,"equal distant props are not final ambiguity");
        require(search.step(scope,103,crowded,no_yield)==SelectionResult::selected,"nearest wins regardless of table order");
        require(search.candidate().body==20+nearest*4096 && search.matches()==3,"nearest identity and candidate count");
        require(search.nearest_distance()==.5f && search.second_distance()==1.f,"distance diagnostics");
    }
    scope.slots=2;search.begin(scope,100);
    require(search.step(scope,101,[](uint32_t i,SelectionCandidate& c){c={10+i,20+i,30+i,i?1.1f:1.f};return true;},no_yield)==SelectionResult::ambiguous,"near ties need repositioning");
    for(float bad:{-1.f,4.1f,std::numeric_limits<float>::quiet_NaN()}) {
        search.begin(scope,100);
        require(search.step(scope,101,[&](uint32_t,SelectionCandidate& c){c={10,20,30,bad};return true;},no_yield)==SelectionResult::invalid,"invalid candidate distance");
    }
    scope.slots=64;search.begin(scope,100);
    require(search.step(scope,101,[](uint32_t,SelectionCandidate&){return false;},no_yield)==SelectionResult::none,"empty nearby search");
    for(auto count:{0u,(1u<<20)+1}) {scope.slots=count;require(search.begin(scope,100)==SelectionResult::invalid,"invalid table bounds");}
    scope.slots=1u<<20;require(search.begin(scope,100)==SelectionResult::pending,"maximum supported table");
    scope.position[0]=std::numeric_limits<float>::quiet_NaN();
    require(search.begin(scope,100)==SelectionResult::invalid,"nonfinite position rejected");
}
int main() {
    try {test_selection();test_target_diagnostics();test_lifetime_diagnostics();} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
    if(!crml::physics::test_session_prologues()) {std::cerr<<"Physics trial hook relocation failed\n";return 1;}
    std::cout<<"Physics nearest selection, bounds, batched search, cancellation, identity changes and hook relocation passed\n";
}
