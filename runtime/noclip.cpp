#include "noclip.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace crml::probe {
Direction camera_relative(Direction input, const CameraBasis& camera) noexcept {
    // Keep WASD horizontal; Space/Ctrl controls altitude independently of camera pitch.
    if(!camera.valid) return {0,input.y,0,input.fast};
    const float length=std::hypot(camera.right[0],camera.right[2]);
    if(!std::isfinite(length) || length<.01f) return {0,input.y,0,input.fast};
    const float rx=camera.right[0]/length, rz=camera.right[2]/length;
    return {input.x*rx-input.z*rz,input.y,input.x*rz+input.z*rx,input.fast};
}
void Flight::reset(StopReason reason,uint64_t now,const Sample* sample) noexcept {
    auto saved=stopped;
    const auto saved_restore=restored;
    auto cancelled=cancelled_guests;
    if(enabled && guest_driven && owner) {
        // request_motion reserves this owner's receipt before acquisition.
        const auto record=std::find_if(cancelled.begin(),cancelled.end(),[&](const auto& r){return r.owner==owner;});
        if(record!=cancelled.end()) {record->reason=reason;record->pending=true;}
    }
    if(enabled) {
        saved.reason=reason; ++saved.count; saved.tick=now; saved.entity=entity; saved.requested=position;
        saved.source="controller";saved.sample_age_ms=0;
        if(sample) std::copy_n(sample->position,3,saved.observed.begin());
        else saved.observed=position;
    }
    *this=Flight{}; stopped=saved; restored=saved_restore;cancelled_guests=cancelled;
}
void Flight::release_owner(uint64_t who,uint64_t now,const Sample* sample) noexcept {
    if(!who) return;
    if(owner==who) reset(StopReason::mod_release,now,sample);
    for(auto& cancelled:cancelled_guests) if(cancelled.owner==who) cancelled={};
}
int Flight::request_sampled_motion(uint64_t who,bool enable,float x,float y,float z,const Sample& sample,uint64_t sample_tick,
    uint64_t now,bool foreground,bool escape,bool input_fresh) noexcept {
    if(owner && owner!=who) return -2;
    const auto before=stopped.count;
    int result=-1;
    // A renewal only publishes intent. The controller still validates the live
    // entity and its own 250 ms step gap before writing a transform. Do not
    // cancel that lease earlier merely because the worker ran between frames.
    const uint64_t limit=enabled && owner==who?250:100;
    const bool sample_fresh=sample_tick && now>=sample_tick && now-sample_tick<=limit;
    if(enable && (!foreground || escape || !sample_fresh)) {
        if(owner==who) reset(!foreground?StopReason::focus:escape?StopReason::escape:StopReason::stale_sample,now,&sample);
    } else result=request_motion(who,enable,x,y,z,sample,now,input_fresh);
    if(stopped.count!=before) {
        stopped.source=!sample_fresh?"player_sample":!input_fresh?"keyboard_sample":"request";
        stopped.sample_age_ms=sample_tick && now>=sample_tick?now-sample_tick:UINT64_MAX;
    }
    return result;
}
int Flight::request_motion(uint64_t who,bool enable,float x,float y,float z,const Sample& sample,uint64_t now,bool input_fresh) noexcept {
    if(!who || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::hypot(x,y,z)>20.f) return -1;
    if(owner && owner!=who) return -2;
    if(!enable) {
        release_owner(who,now,&sample);return 0;
    }
    if(!input_fresh) {reset(StopReason::stale_sample,now,&sample);return -1;}
    auto receipt=std::find_if(cancelled_guests.begin(),cancelled_guests.end(),[&](const auto& r){return r.owner==who;});
    if(!enabled && receipt!=cancelled_guests.end() && receipt->pending) {receipt->pending=false;return -1;}
    if(receipt==cancelled_guests.end()) {
        receipt=std::find_if(cancelled_guests.begin(),cancelled_guests.end(),[](const auto& r){return !r.owner;});
        // Retain acknowledged reasons while space permits. Pending cancellations
        // are never evicted; acknowledgement retains its legacy capacity effect.
        if(receipt==cancelled_guests.end())
            receipt=std::find_if(cancelled_guests.begin(),cancelled_guests.end(),[](const auto& r){return !r.pending;});
        if(receipt==cancelled_guests.end()) return -1;
    }
    if(enabled && (now<lease || now-lease>500)) {reset(StopReason::lease,now,&sample);return -1;}
    if(enabled && (entity!=sample.entity || world!=sample.world)) {reset(entity!=sample.entity?StopReason::entity:StopReason::world,now,&sample);return -1;}
    if(!sample.entity || !sample.world || sample.disabled || sample.teleported || sample.keyframed[0] || sample.keyframed[1]) {
        reset(sample.disabled?StopReason::disabled:sample.teleported?StopReason::teleport:StopReason::keyframed,now,&sample);return -1;
    }
    if(!enabled) {owner=who;entity=sample.entity;world=sample.world;enabled=true;last_step=0;}
    guest_driven=true;lease=now;speed=20.f;
    *receipt={who,StopReason::none,false};
    requested={x/20.f,y/20.f,z/20.f,false};
    return 1;
}
int Flight::read_motion(uint64_t who,crml_motion_state& out) const noexcept {
    out={};if(!who) return -1;
    out.version=1;
    if(enabled && guest_driven && owner==who) {out.state=CRML_MOTION_ACTIVE;return 1;}
    const auto record=std::find_if(cancelled_guests.begin(),cancelled_guests.end(),[&](const auto& r){return r.owner==who;});
    if(record==cancelled_guests.end() || record->reason==StopReason::none) return 1;
    out.state=CRML_MOTION_STOPPED;
    out.flags=record->pending?CRML_MOTION_CANCEL_PENDING:0u;
    // Public values deliberately do not depend on the diagnostic enum's order.
    switch(record->reason) {
    case StopReason::focus: out.stop_reason=CRML_MOTION_STOP_FOCUS;break;
    case StopReason::escape: out.stop_reason=CRML_MOTION_STOP_ESCAPE;break;
    case StopReason::stale_sample: out.stop_reason=CRML_MOTION_STOP_STALE_SAMPLE;break;
    case StopReason::lease: out.stop_reason=CRML_MOTION_STOP_LEASE;break;
    case StopReason::entity: out.stop_reason=CRML_MOTION_STOP_PLAYER_CHANGED;break;
    case StopReason::world: out.stop_reason=CRML_MOTION_STOP_WORLD_CHANGED;break;
    case StopReason::disabled: out.stop_reason=CRML_MOTION_STOP_CONTROLLER_DISABLED;break;
    case StopReason::teleport: out.stop_reason=CRML_MOTION_STOP_TELEPORT;break;
    case StopReason::keyframed: out.stop_reason=CRML_MOTION_STOP_KEYFRAMED;break;
    case StopReason::tick_gap: out.stop_reason=CRML_MOTION_STOP_TICK_GAP;break;
    case StopReason::speed: out.stop_reason=CRML_MOTION_STOP_INVALID_SPEED;break;
    case StopReason::displacement: out.stop_reason=CRML_MOTION_STOP_DISPLACEMENT;break;
    case StopReason::direction: out.stop_reason=CRML_MOTION_STOP_INVALID_DIRECTION;break;
    case StopReason::coordinates: out.stop_reason=CRML_MOTION_STOP_INVALID_COORDINATES;break;
    case StopReason::view: out.stop_reason=CRML_MOTION_STOP_INVALID_VIEW;break;
    case StopReason::mod_release: out.stop_reason=CRML_MOTION_STOP_RELEASE;break;
    case StopReason::shutdown: out.stop_reason=CRML_MOTION_STOP_SHUTDOWN;break;
    default: out.stop_reason=CRML_MOTION_STOP_OTHER;break;
    }
    return 1;
}
bool Flight::step(const Sample& sample, uint64_t current_world, uint64_t now, bool focused, bool input_fresh,
                  Direction input, std::array<float, 3>& target) noexcept {
    if (!enabled) return false;
    const auto stop=[&](StopReason reason) { reset(reason,now,&sample); return false; };
    if(!focused) return stop(StopReason::focus);
    if(!input_fresh) return stop(StopReason::stale_sample);
    if(now<lease || now-lease>500) return stop(StopReason::lease);
    if(sample.entity!=entity) return stop(StopReason::entity);
    if(current_world!=world) return stop(StopReason::world);
    if(sample.disabled) return stop(StopReason::disabled);
    if(teleport_blocks(sample.teleported)) return stop(StopReason::teleport);
    if(sample.keyframed[0] || sample.keyframed[1]) return stop(StopReason::keyframed);
    if(last_step && (now<last_step || now-last_step>250)) return stop(StopReason::tick_gap);
    if(!std::isfinite(speed) || speed<.25f || speed>20) return stop(StopReason::speed);
    const float dt = last_step ? std::min(float(now - last_step) / 1000.f, .05f) : 0.f;
    last_step = now;
    if(!positioned) {
        std::copy_n(sample.position,3,position.begin()); positioned=true;
    }
    // Once flight owns a position, a same-player/world teleport must not surrender
    // control to an out-of-bounds reset. Identity, lease and controller gates above
    // still apply. Unflagged large relocations remain a separate cancellation.
    float discrepancy{};
    for(int i=0;i<3;++i) {
        const float delta=sample.position[i]-position[i]; discrepancy+=delta*delta;
    }
    if(!std::isfinite(discrepancy) || (discrepancy>25.f && !sample.teleported)) return stop(StopReason::displacement);
    const float length = std::sqrt(input.x*input.x + input.y*input.y + input.z*input.z);
    if (!std::isfinite(length)) return stop(StopReason::direction);
    const float distance = speed * (input.fast ? 3.f : 1.f) * dt / std::max(length, 1.f);
    for (int i=0; i<3; ++i) {
        const float axis = i==0 ? input.x : i==1 ? input.y : input.z;
        target[i] = position[i] + axis * distance;
        if (!std::isfinite(target[i])) return stop(StopReason::coordinates);
    }
    if(sample.teleported && discrepancy>.0001f) {
        ++restored.count; restored.tick=now; restored.requested=target;
        std::copy_n(sample.position,3,restored.observed.begin());
    }
    position=target;
    return true;
}

bool Override::prepare(const void* original, const Sample& sample, const std::array<float,3>& target) noexcept {
    __try {
        std::memcpy(view.data(), original, sizeof(view));
        if (view[18] != sample.row || sample.row >= 16384) return false;
        std::memcpy(transform.data(), reinterpret_cast<void*>(view[11] + sample.row*32ull), 32);
        std::memcpy(pushability.data(), reinterpret_cast<void*>(view[8] + sample.row*8ull), pushability.size());
        // The core still runs its contact/push solver after the keyframed branch
        // (0x1b99311 -> 0x2d79fb0). Disable that pass for this player call only.
        pushability[0]=0;
        for (int i=0; i<3; ++i) transform[4+i] = target[i];
        // These are integer ABI anchors, not C++ array pointers to dereference here.
        // The verified native routine adds row*stride before reading each component.
        view[11] = reinterpret_cast<uintptr_t>(transform.data()) - sample.row*32ull;
        view[7] = reinterpret_cast<uintptr_t>(keyframed.data()) - sample.row*2ull;
        view[8] = reinterpret_cast<uintptr_t>(pushability.data()) - sample.row*8ull;
        // The teleport branch precedes keyframed movement (0x1b99030). Keep this
        // invocation on the noclip path without changing the real teleport flag.
        view[14] = reinterpret_cast<uintptr_t>(teleported.data()) - sample.row;
        return true;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
}
