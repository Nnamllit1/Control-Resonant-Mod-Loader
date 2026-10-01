#pragma once
#include "movement_view.h"
#include "../sdk/include/crml_state.h"
#include <cmath>
#include <limits>

namespace crml::probe {
// Caller serializes access. Only publish() receives an authenticated, copied
// controller sample; read() never follows engine memory or returns its handles.
class PlayerSnapshot {
public:
    void invalidate() noexcept { valid_=false; }
    void publish(const Sample& sample,uint64_t now) noexcept {
        if(!sample.world || !sample.entity) {invalidate();return;}
        for(const float coordinate:sample.position)
            if(!std::isfinite(coordinate)) {invalidate();return;}
        if(!valid_ || sample.world!=world_ || sample.entity!=entity_ || now<tick_ || now-tick_>500) {
            // Exhaustion must never recycle an identity visible to a guest.
            if(generation_==std::numeric_limits<uint64_t>::max()) {invalidate();return;}
            ++generation_;
        }
        world_=sample.world;entity_=sample.entity;tick_=now;
        for(unsigned i=0;i<3;++i) position_[i]=sample.position[i];
        valid_=true;
    }
    int read(crml_player_state& out,uint64_t now,bool foreground) const noexcept {
        out={};
        if(!foreground || !valid_ || now<tick_ || now-tick_>500) return 0;
        out.version=1;out.age_ms=static_cast<uint32_t>(now-tick_);out.generation=generation_;
        for(unsigned i=0;i<3;++i) out.position[i]=position_[i];
        return 1;
    }
private:
    uintptr_t world_{};
    uint64_t entity_{},tick_{},generation_{};
    float position_[3]{};
    bool valid_{};
};
}
