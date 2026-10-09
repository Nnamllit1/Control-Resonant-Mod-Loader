#pragma once
#include "player_snapshot.h"
#include "navigation_view.h"
#include <limits>

namespace crml::navigation {
// Called under the movement bridge's sample lock. Engine reads happen in
// inspect_ground at the native callback; read() only copies published data.
class Snapshot {
public:
    void invalidate() noexcept {
        player_.invalidate();flags_=0;observed_=false;bump_continuity();
    }
    void publish(const probe::Sample& sample,const GroundObservation* ground,uint64_t now) noexcept {
        player_.publish(sample,now);
        crml_player_state player{};
        if(player_.read(player,now,true)!=1 || sequence_==UINT64_MAX){invalidate();return;}
        if(!observed_) {
            if(!continuity_)bump_continuity();
        } else if(sample.world!=world_ || sample.entity!=entity_ || now<tick_) {
            bump_continuity();
        }
        world_=sample.world;entity_=sample.entity;tick_=now;observed_=true;
        ++sequence_;flags_=0;up_[0]=up_[1]=up_[2]=0;
        if(sample.teleported)flags_|=CRML_NAV_TELEPORTED;
        if(sample.disabled)flags_|=CRML_NAV_CONTROLLER_DISABLED;
        if(sample.keyframed[0] || sample.keyframed[1])flags_|=CRML_NAV_KEYFRAMED;
        if(ground) {
            double norm{};for(float component:ground->up)norm+=static_cast<double>(component)*component;
            if(std::isfinite(norm) && norm>=0.98 && norm<=1.02) {
                flags_|=CRML_NAV_UP_VALID;for(unsigned i=0;i<3;++i)up_[i]=ground->up[i];
            }
        }
    }
    int read(crml_navigation_state& out,uint64_t now,bool foreground) const noexcept {
        out={};crml_player_state player{};
        if(player_.read(player,now,foreground)!=1)return 0;
        out.version=1;out.age_ms=player.age_ms;out.generation=player.generation;
        out.sequence=sequence_;out.flags=flags_;
        for(unsigned i=0;i<3;++i){out.position[i]=player.position[i];out.up[i]=up_[i];}
        return 1;
    }
    int read_v2(crml_navigation_state_v2& out,uint64_t now,bool foreground) const noexcept {
        out={};
        if(continuity_exhausted_ || !continuity_)return 0;
        crml_navigation_state state{};
        if(read(state,now,foreground)!=1)return 0;
        out.version=2;out.age_ms=state.age_ms;out.generation=state.generation;
        out.sequence=state.sequence;out.flags=state.flags;
        for(unsigned i=0;i<3;++i){out.position[i]=state.position[i];out.up[i]=state.up[i];}
        out.continuity=continuity_;
        return 1;
    }
private:
    void bump_continuity() noexcept {
        if(continuity_exhausted_)return;
        if(continuity_==std::numeric_limits<uint64_t>::max()){
            continuity_exhausted_=true;return;
        }
        ++continuity_;
    }
    probe::PlayerSnapshot player_;
    uint64_t sequence_{};
    uintptr_t world_{};
    uint64_t entity_{},tick_{},continuity_{};
    bool observed_{},continuity_exhausted_{};
    uint32_t flags_{};
    float up_[3]{};
};
}
