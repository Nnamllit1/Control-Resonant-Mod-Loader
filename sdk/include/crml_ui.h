#pragma once
#include <stdint.h>

// Copied native UI observation. No DOM objects, scripts or engine pointers.
#define CRML_UI_STATE_VERSION 1u
#define CRML_UI_STATE_SIZE 32u
#define CRML_UI_SCREEN_NONE 0u
#define CRML_UI_SCREEN_PHOTOSENSITIVITY 1u
#define CRML_UI_SCREEN_LEGAL 2u
#define CRML_UI_SCREEN_EULA 3u
#define CRML_UI_SCREEN_PRIVACY 4u
#define CRML_UI_SCREEN_SAVE_WARNING 5u
#define CRML_UI_SCREEN_DISPLAY_CALIBRATION 6u
#define CRML_UI_SCREEN_FIRST_TIME_SETUP 7u
#define CRML_UI_SCREEN_USER_INTERACTION 8u
#define CRML_UI_SCREEN_MAIN_MENU 9u
#define CRML_UI_TARGET_ID 1u
#define CRML_UI_TARGET_CLASS 2u
#define CRML_UI_TARGET_NAME_MAX 64u
#define CRML_UI_PRESENT_MAX_MS 1000u
#define CRML_UI_ACTION_CONTINUE 1u
#define CRML_UI_ACTION_MASK_CONTINUE 1u

// Development API. Status is about command delivery, not an engine transition.
#define CRML_UI_ACTION_QUEUED 1
#define CRML_UI_ACTION_DELIVERED 2
#define CRML_UI_ACTION_DISPATCHED 3
#define CRML_UI_ACTION_SKIPPED 4
#define CRML_UI_ACTION_DISPATCH_FAILED 5
#define CRML_UI_ACTION_EXPIRED 6
#define CRML_UI_ACTION_OUTCOME_UNKNOWN 7
#define CRML_UI_ACTION_CANCELLED 8

typedef struct crml_ui_state {
    uint32_t size;
    uint32_t version;
    uint32_t screen;
    uint32_t actions;
    uint64_t generation;
    uint32_t age_ms;
    uint32_t reserved;
} crml_ui_state;

#ifdef __cplusplus
static_assert(sizeof(crml_ui_state) == CRML_UI_STATE_SIZE);
#endif
