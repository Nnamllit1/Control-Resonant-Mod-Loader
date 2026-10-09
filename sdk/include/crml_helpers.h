#pragma once
#include "crml.h"

// Descriptive C helpers over existing ABI 1 imports. No additional permission,
// allocation, retry, state or host call. Underlying limits/results still apply.
// Including crml.h also makes these helpers available.

// player.motion: noncolliding character velocity, not a free camera or teleport.
static inline int32_t crml_player_flight_set_velocity(float x,float y,float z) {
    return crml_motion_set(1,x,y,z);
}
static inline int32_t crml_player_flight_release(void) {
    return crml_motion_set(0,0,0,0);
}
// This observation import needs runtime >=0.1.0-alpha.4.4.dev.0.
static inline int32_t crml_player_flight_read(crml_motion_state* out) {
    return crml_motion_read(out,sizeof(*out));
}

// player.visibility: current player root mesh only. Does not change AI/collision.
static inline int32_t crml_player_mesh_set_hidden(int32_t hidden) {
    return crml_visibility_set(hidden);
}
// This observation import needs runtime >=0.1.0-alpha.4.4.dev.0.
static inline int32_t crml_player_mesh_read(crml_visibility_state* out) {
    return crml_visibility_read(out,sizeof(*out));
}

// physics.damping: linear damping on the selected eligible prop. Not friction,
// weight, angular damping, or an impulse. Target/value/duration remain guest policy.
static inline int32_t crml_prop_set_linear_damping(uint64_t target,float value,uint32_t duration_ms) {
    return crml_physics_apply(target,value,duration_ms);
}
// Requests restoration AND cancels this owner's pending selection/application.
// Completion still requires physics_status; other owners are unaffected.
static inline int32_t crml_prop_restore_linear_damping(void) {
    return crml_physics_restore();
}

// ui.read/media.read: reviewed startup adapters, not a general engine UI/video API.
static inline int32_t crml_startup_read_screen(crml_ui_state* out) {
    return crml_ui_read(out,sizeof(*out));
}
static inline int32_t crml_startup_read_media(crml_media_state* out) {
    return crml_media_read(out,sizeof(*out));
}
static inline int32_t crml_startup_skip_media(uint64_t generation) {
    return crml_media_skip(generation);
}

// ui.presentation: temporarily hide one existing ID/class target. This does not
// construct a panel, accept selectors/HTML, or guarantee that a target exists.
static inline int32_t crml_ui_element_set_hidden(uint64_t generation,uint32_t kind,
        const char* name,uint32_t name_len,uint32_t hidden,uint32_t duration_ms) {
    return crml_ui_present(generation,kind,name,name_len,hidden,duration_ms);
}
