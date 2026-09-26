#include "physics_trial.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace crml::physics;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Backend final : DampingBackend {
    Target live{1,0x200000001,0x400000003,0};
    float value{.25f};
    bool available{true}, fail_call{}, ignore_write{}, retire_on_write{}, hide_after_write{}, concurrent_change{};
    unsigned writes{}, reads{};
    Reading read(const Target& target) noexcept override {
        ++reads;
        if(target!=live) return {ReadStatus::retired,0};
        return available?Reading{ReadStatus::ok,value}:Reading{};
    }
    bool write(const Target& target,float expected,float replacement) noexcept override {
        ++writes;
        if(concurrent_change) value=3;
        if(!available || target!=live || value!=expected) return false;
        if(!ignore_write) value=replacement;
        if(retire_on_write) ++live.epoch;
        if(hide_after_write) available=false;
        return !fail_call;
    }
};
int main() {
    try {
        Backend b; DampingTrial trial;
        require(trial.apply(b,b.live,8,100,500)==TrialResult::applied,"apply and read back");
        require(b.value==8 && b.writes==1 && trial.target()==b.live,"retain only target identity");
        require(trial.apply(b,b.live,9,200,500)==TrialResult::busy && b.writes==1,"no replacement while restoration is owed");
        require(trial.poll(b,599,true)==TrialResult::active && b.writes==1,"no repeated setter during active interval");
        require(trial.poll(b,600,true)==TrialResult::restored && b.value==.25f && !trial.pending(),"deadline restores original");
        require(trial.target()==Target{},"finished trial drops identity");

        require(trial.apply(b,b.live,8,1000,500)==TrialResult::applied,"second trial");
        b.value=2;
        auto writes=b.writes;
        require(trial.poll(b,1001,false)==TrialResult::conflict && b.value==2 && b.writes==writes,"preserve later engine override");
        require(!trial.pending(),"conflict ends ownership");

        b.value=.25f;
        require(trial.apply(b,b.live,8,2000,500)==TrialResult::applied,"reload setup");
        ++b.live.epoch; b.value=1;
        writes=b.writes;
        require(trial.poll(b,2001,false)==TrialResult::retired && b.value==1 && b.writes==writes,"world reload never restores into replacement");
        require(trial.apply(b,b.live,8,2100,500)==TrialResult::applied,"body replacement setup");
        b.live.body+=uint64_t(2)<<32; b.value=3;
        writes=b.writes;
        require(trial.poll(b,2101,false)==TrialResult::retired && b.writes==writes,"body generation replacement cancels");

        b.value=.25f; b.fail_call=true;
        require(trial.apply(b,b.live,8,3000,500)==TrialResult::restore_pending && b.value==8,"failed call may already have changed the value");
        b.fail_call=false;
        require(trial.poll(b,3001,true)==TrialResult::restored && b.value==.25f,"uncertain apply restores even while held");
        b.hide_after_write=true;
        require(trial.apply(b,b.live,8,4000,500)==TrialResult::restore_pending,"missing readback preserves restoration state");
        require(trial.poll(b,4001,false)==TrialResult::restore_pending && trial.pending(),"unavailable is not retirement");
        b.available=true; b.hide_after_write=false;
        require(trial.poll(b,4002,true)==TrialResult::restored,"restoration request stays latched");

        require(trial.apply(b,b.live,8,5000,500)==TrialResult::applied,"failed restore setup");
        b.ignore_write=true;
        require(trial.poll(b,5001,false)==TrialResult::restore_pending && b.value==8,"refused restore must not report success");
        b.ignore_write=false;
        require(trial.poll(b,5002,true)==TrialResult::restored,"retry refused restore");
        b.ignore_write=true;
        require(trial.apply(b,b.live,8,6000,500)==TrialResult::refused && !trial.pending(),"refused apply clears state only after baseline readback");
        b.ignore_write=false;
        require(trial.apply(b,b.live,8,7000,500)==TrialResult::applied,"clock rollback setup");
        require(trial.poll(b,6999,true)==TrialResult::restored,"clock rollback ends trial");

        b.concurrent_change=true;
        require(trial.apply(b,b.live,8,7100,500)==TrialResult::restore_pending && b.value==3,"adapter compare prevents overwriting a changed baseline");
        b.concurrent_change=false; writes=b.writes;
        require(trial.poll(b,7101,false)==TrialResult::conflict && b.writes==writes,"uncertain apply preserves concurrent change");
        b.value=.25f;
        require(trial.apply(b,b.live,8,7200,500)==TrialResult::applied,"uncertain restore setup");
        b.fail_call=true;
        require(trial.poll(b,7201,false)==TrialResult::restore_pending && b.value==.25f,"failed restore call may have succeeded");
        b.fail_call=false; writes=b.writes;
        require(trial.poll(b,7202,false)==TrialResult::restored && b.writes==writes,"readback confirms restoration without another write");
        b.retire_on_write=true;
        require(trial.apply(b,b.live,8,7300,500)==TrialResult::retired && !trial.pending(),"retirement after write never retains replacement");
        b.retire_on_write=false; b.value=.25f;

        writes=b.writes;
        require(trial.apply(b,b.live,.25f,8000,500)==TrialResult::unchanged && b.writes==writes,"same value does not call setter");
        require(trial.apply(b,b.live,-1,8000,500)==TrialResult::invalid,"negative rejected");
        require(trial.apply(b,b.live,std::numeric_limits<float>::quiet_NaN(),8000,500)==TrialResult::invalid,"NaN rejected");
        require(trial.apply(b,b.live,101,8000,500)==TrialResult::invalid,"bounded requested damping");
        require(trial.apply(b,b.live,8,8000,10001)==TrialResult::invalid,"bounded duration");
        require(trial.apply(b,b.live,8,UINT64_MAX,1)==TrialResult::invalid,"deadline overflow rejected");
        Target invalid=b.live; invalid.epoch=0;
        require(trial.apply(b,invalid,8,8000,500)==TrialResult::invalid,"world incarnation required");
        require(b.writes==writes,"invalid requests never call setter");
        b.value=std::numeric_limits<float>::infinity();
        require(trial.apply(b,b.live,8,8000,500)==TrialResult::unavailable,"invalid baseline is not restorable");
        require(trial.poll(b,8001,false)==TrialResult::idle,"idle does no work");
        std::cout<<"Damping trial restoration, conflicts, retirement and uncertain writes passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
