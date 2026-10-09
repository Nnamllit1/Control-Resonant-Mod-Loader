#pragma once
#include <stdint.h>

// Fixed-width snapshot layouts shared by the native bridge and wasm32 SDK.
// Versioned states. No engine pointers or native entity handles cross this boundary.
typedef struct crml_player_state {
    uint32_t version;
    uint32_t age_ms;
    uint64_t generation;
    float position[3];
    uint32_t reserved;
} crml_player_state;

// One copied movement-callback observation. Generation is transient and must
// not be used as a saved zone/campaign identity. Up is movement-plane local +Y,
// not a camera axis, contact normal or proof that the player is grounded.
typedef struct crml_navigation_state {
    uint32_t version, age_ms;
    uint64_t generation, sequence;
    uint32_t flags, reserved;
    float position[3], up[3];
} crml_navigation_state;
// Additive navigation snapshot. The first 56 bytes match v1 exactly.
// Continuity changes on observed world/player replacement, invalidation, or
// clock rollback; a sampling gap alone does not change it. It is an ephemeral
// observation token, not a persistent world, entity, zone, or save identity.
typedef struct crml_navigation_state_v2 {
    uint32_t version, age_ms;
    uint64_t generation, sequence;
    uint32_t flags, reserved;
    float position[3], up[3];
    uint64_t continuity;
} crml_navigation_state_v2;
#define CRML_NAV_UP_VALID 1u
#define CRML_NAV_TELEPORTED 2u
#define CRML_NAV_CONTROLLER_DISABLED 4u
#define CRML_NAV_KEYFRAMED 8u

typedef struct crml_physics_state {
    uint32_t version;
    uint32_t age_ms;
    uint32_t flags;
    uint32_t reserved;
    float linear_damping;
    float angular_damping;
    float linear_speed;
    float angular_speed;
} crml_physics_state;
#define CRML_PHYSICS_STATE_DAMPING 1u
#define CRML_PHYSICS_STATE_SPEED 2u

typedef struct crml_camera_state {
    uint32_t version;
    uint32_t age_ms;
    uint64_t generation;
    int32_t mode;
    uint32_t flags;
    float position[3];
    float basis[9];
    float horizontal_fov_radians;
    float aspect_ratio;
} crml_camera_state;
#define CRML_CAMERA_STATE_POSE 1u
#define CRML_CAMERA_STATE_LENS 2u

// Owner-local motion_set lease observation. ACTIVE means accepted by the
// adapter, not proof that the engine applied the velocity.
typedef struct crml_motion_state {
    uint32_t version;
    uint32_t state;
    uint32_t stop_reason;
    uint32_t flags;
} crml_motion_state;
#define CRML_MOTION_IDLE 0u
#define CRML_MOTION_ACTIVE 1u
#define CRML_MOTION_STOPPED 2u
#define CRML_MOTION_CANCEL_PENDING 1u
#define CRML_MOTION_STOP_NONE 0u
#define CRML_MOTION_STOP_FOCUS 1u
#define CRML_MOTION_STOP_ESCAPE 2u
#define CRML_MOTION_STOP_STALE_SAMPLE 3u
#define CRML_MOTION_STOP_LEASE 4u
#define CRML_MOTION_STOP_PLAYER_CHANGED 5u
#define CRML_MOTION_STOP_WORLD_CHANGED 6u
#define CRML_MOTION_STOP_CONTROLLER_DISABLED 7u
#define CRML_MOTION_STOP_TELEPORT 8u
#define CRML_MOTION_STOP_KEYFRAMED 9u
#define CRML_MOTION_STOP_TICK_GAP 10u
#define CRML_MOTION_STOP_INVALID_SPEED 11u
#define CRML_MOTION_STOP_DISPLACEMENT 12u
#define CRML_MOTION_STOP_INVALID_DIRECTION 13u
#define CRML_MOTION_STOP_INVALID_COORDINATES 14u
#define CRML_MOTION_STOP_INVALID_VIEW 15u
#define CRML_MOTION_STOP_RELEASE 16u
#define CRML_MOTION_STOP_SHUTDOWN 17u
#define CRML_MOTION_STOP_OTHER 255u

// Lease-local visibility evidence, not a render completion fence.
typedef struct crml_visibility_state {
    uint32_t version;
    uint32_t state;
    uint32_t flags;
    uint32_t remaining_ms;
} crml_visibility_state;
#define CRML_VISIBILITY_IDLE 0u
#define CRML_VISIBILITY_ACTIVE 1u
#define CRML_VISIBILITY_EXPIRED 2u
#define CRML_VISIBILITY_OBSERVED_HIDDEN 1u
#define CRML_VISIBILITY_SUBMITTED 2u

#ifdef __cplusplus
static_assert(sizeof(crml_player_state)==32);
static_assert(sizeof(crml_navigation_state)==56);
static_assert(sizeof(crml_navigation_state_v2)==64);
static_assert(sizeof(crml_physics_state)==32);
static_assert(sizeof(crml_camera_state)==80);
static_assert(sizeof(crml_motion_state)==16);
static_assert(sizeof(crml_visibility_state)==16);
#endif
