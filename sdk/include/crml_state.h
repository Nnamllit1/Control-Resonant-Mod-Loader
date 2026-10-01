#pragma once
#include <stdint.h>

// Fixed-width snapshot layouts shared by the native bridge and wasm32 SDK.
// Version 1. No engine pointers or native entity handles cross this boundary.
typedef struct crml_player_state {
    uint32_t version;
    uint32_t age_ms;
    uint64_t generation;
    float position[3];
    uint32_t reserved;
} crml_player_state;

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

#ifdef __cplusplus
static_assert(sizeof(crml_player_state)==32);
static_assert(sizeof(crml_physics_state)==32);
static_assert(sizeof(crml_camera_state)==80);
#endif
