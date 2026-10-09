#include "navigation_snapshot.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
bool cleared(const crml_navigation_state& value) {
    const crml_navigation_state zero{};return std::memcmp(&value,&zero,sizeof(value))==0;
}
bool cleared(const crml_navigation_state_v2& value) {
    const crml_navigation_state_v2 zero{};return std::memcmp(&value,&zero,sizeof(value))==0;
}
int main() {
    try {
        static_assert(offsetof(crml_navigation_state_v2,continuity)==56);
        crml::navigation::Snapshot snapshot;
        crml_navigation_state state{};
        crml_navigation_state_v2 state2{};
        require(snapshot.read(state,100,true)==0 && cleared(state),"empty observation not cleared");
        require(snapshot.read_v2(state2,100,true)==0 && cleared(state2),"empty v2 observation not cleared");
        crml::probe::Sample sample{};sample.world=1;sample.entity=2;
        sample.position[0]=3;sample.position[1]=4;sample.position[2]=5;
        crml::navigation::GroundObservation ground{};ground.up[0]=-1;
        snapshot.publish(sample,&ground,100);
        require(snapshot.read(state,112,true)==1 && state.version==1 && state.age_ms==12 &&
            state.generation==1 && state.sequence==1 && state.flags==CRML_NAV_UP_VALID &&
            state.position[2]==5 && state.up[0]==-1 && state.reserved==0,"copied navigation layout differs");
        require(snapshot.read_v2(state2,112,true)==1 && state2.version==2 && state2.continuity==1 &&
            state2.age_ms==state.age_ms && state2.generation==state.generation &&
            state2.sequence==state.sequence && state2.flags==state.flags &&
            state2.position[2]==state.position[2] && state2.up[0]==state.up[0],"copied v2 layout differs");
        sample.position[0]=99;ground.up[0]=1;
        require(snapshot.read(state,112,true)==1 && state.position[0]==3 && state.up[0]==-1,"snapshot retained live references");
        require(snapshot.read(state,600,true)==1,"500ms freshness boundary rejected");
        require(snapshot.read(state,601,true)==0 && cleared(state),"expired observation leaked");
        require(snapshot.read_v2(state2,601,true)==0 && cleared(state2),"expired v2 observation leaked");
        require(snapshot.read(state,99,true)==0 && cleared(state),"clock rollback leaked");
        require(snapshot.read_v2(state2,99,true)==0 && cleared(state2),"clock rollback v2 read leaked");
        require(snapshot.read(state,100,false)==0 && cleared(state),"background observation leaked");
        require(snapshot.read_v2(state2,100,false)==0 && cleared(state2),"background v2 observation leaked");
        sample.teleported=1;sample.disabled=1;sample.keyframed[1]=1;
        snapshot.publish(sample,nullptr,120);
        require(snapshot.read(state,120,true)==1 && state.sequence==2 && state.generation==1 &&
            state.flags==(CRML_NAV_TELEPORTED|CRML_NAV_CONTROLLER_DISABLED|CRML_NAV_KEYFRAMED) &&
            state.up[0]==0 && state.up[1]==0 && state.up[2]==0,"invalid up or discontinuity flags differ");
        ground.up[0]=NAN;snapshot.publish(sample,&ground,130);
        require(snapshot.read(state,130,true)==1 && !(state.flags&CRML_NAV_UP_VALID) && state.up[0]==0,"nonfinite up published");
        ground.up[0]=2;snapshot.publish(sample,&ground,140);
        require(snapshot.read(state,140,true)==1 && !(state.flags&CRML_NAV_UP_VALID),"nonunit up published");
        sample.world=3;snapshot.publish(sample,nullptr,150);
        require(snapshot.read(state,150,true)==1 && state.generation==2,"world replacement did not break continuity");
        require(snapshot.read_v2(state2,150,true)==1 && state2.continuity==2,"world replacement did not change v2 continuity");
        sample.entity=4;snapshot.publish(sample,nullptr,160);
        require(snapshot.read(state,160,true)==1 && state.generation==3,"player replacement did not break continuity");
        require(snapshot.read_v2(state2,160,true)==1 && state2.continuity==3,"player replacement did not change v2 continuity");
        snapshot.invalidate();require(snapshot.read(state,160,true)==0 && cleared(state),"invalidation leaked data");
        require(snapshot.read_v2(state2,160,true)==0 && cleared(state2),"invalidation leaked v2 data");
        snapshot.publish(sample,nullptr,170);
        require(snapshot.read(state,170,true)==1 && state.generation==4 && state.sequence==7,"resume recycled continuity");
        require(snapshot.read_v2(state2,170,true)==1 && state2.continuity==4,"explicit invalidation did not change v2 continuity");
        snapshot.publish(sample,nullptr,800);
        require(snapshot.read(state,800,true)==1 && state.generation==5,"publication gap did not break continuity");
        require(snapshot.read_v2(state2,800,true)==1 && state2.continuity==4,
            "publication gap incorrectly changed v2 continuity");
        snapshot.publish(sample,nullptr,790);
        require(snapshot.read(state,790,true)==1 && state.generation==6,"publication clock rollback did not break continuity");
        require(snapshot.read_v2(state2,790,true)==1 && state2.continuity==5,
            "publication clock rollback did not change v2 continuity");
        sample.position[1]=INFINITY;snapshot.publish(sample,nullptr,810);
        require(snapshot.read(state,810,true)==0 && cleared(state),"invalid player position published");
        require(snapshot.read_v2(state2,810,true)==0 && cleared(state2),"invalid player position published in v2");
        sample.position[1]=4;snapshot.publish(sample,nullptr,820);
        require(snapshot.read_v2(state2,820,true)==1 && state2.continuity==6,
            "invalid sample did not break v2 continuity");
        std::cout<<"Navigation snapshot checks passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
