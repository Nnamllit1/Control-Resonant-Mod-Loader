#pragma once
#include <stdint.h>

#define CRML_SETTING_BOOL 1u
#define CRML_SETTING_INT 2u
#define CRML_SETTING_NUMBER 3u
#define CRML_SETTING_TEXT 4u
#define CRML_SETTINGS_MAX 32u
#define CRML_SETTING_TEXT_MAX 255u

// ABI 1, descriptor version 1. NUL-terminated UTF-8 strings; key is lowercase
// ASCII [a-z0-9_-], 1..31 bytes. Label must be nonempty. Zero-initialize first.
// Bool: min=0,max=1,step=1. Numbers: finite +/-1e9, min<max, positive step,
// at most 1e6 steps, default on the step grid. Integer fields must be integral.
typedef struct crml_setting_definition {
    uint32_t version, kind;
    char key[32], label[96], description[192];
    double initial, minimum, maximum, step;
} crml_setting_definition;

// Handles belong to this loaded mod only. Revisions start at 1 and change only
// when the value changes. An array is returned by settings_read in handle order.
typedef struct crml_setting_value {
    uint32_t handle, kind;
    double value;
    uint64_t revision;
} crml_setting_value;

// Separate additive ABI: existing numeric descriptors and snapshots keep their
// layouts. UTF-8 single-line values; max_bytes excludes the terminating NUL.
typedef struct crml_text_setting_definition {
    uint32_t version, max_bytes;
    char key[32], label[96], description[192], initial[256];
} crml_text_setting_definition;

typedef struct crml_text_setting_value {
    uint32_t version, handle;
    uint64_t revision;
    uint32_t max_bytes, length;
    char value[256];
} crml_text_setting_value;
