#pragma once
#include <stdint.h>

// Stable ABI 1 capability bits. A bit is not an address or a grant of access.
#define CRML_CAP_LOG               (1u << 0)
#define CRML_CAP_INPUT_BUTTONS     (1u << 1)
#define CRML_CAP_PLAYER_NOCLIP     (1u << 2)
#define CRML_CAP_PLAYER_VISIBILITY (1u << 3)
#define CRML_CAP_PHYSICS_DAMPING    (1u << 4)
#define CRML_CAP_INPUT_MOTION      (1u << 5)
// The current player.motion service is explicitly noncolliding.
#define CRML_CAP_PLAYER_MOTION     (1u << 6)
#define CRML_CAP_INPUT_ACTIONS     (1u << 7)
#define CRML_CAP_PLAYER_READ       (1u << 8)
#define CRML_CAP_CAMERA_READ       (1u << 9)
#define CRML_CAP_UI_READ           (1u << 10)
#define CRML_CAP_UI_ACTIVATE       (1u << 11)
#define CRML_CAP_MEDIA_READ        (1u << 12)
#define CRML_CAP_MEDIA_SKIP        (1u << 13)
#define CRML_CAP_UI_PRESENTATION   (1u << 14)
#define CRML_CAP_STORAGE           (1u << 15)
#define CRML_CAP_SETTINGS          (1u << 16)
#define CRML_CAP_FEEDBACK          (1u << 17)
#define CRML_CAP_TUTORIALS         (1u << 18)
#define CRML_CAP_NAVIGATION_READ   (1u << 19)
#define CRML_CAP_DRAWING           (1u << 20)
#define CRML_CAP_LISTS             (1u << 21)
#define CRML_CAP_ACTION_RULES      (1u << 22)
#define CRML_ACTION_COUNT 16u
