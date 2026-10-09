#pragma once
#include "crml_abi.h"
#include <stddef.h>

#define CRML_INPUT_STATE_VERSION 1u
#define CRML_INPUT_KEY_NAME_CAPACITY 12u
#define CRML_INPUT_AVAILABLE     (1u << 0)
#define CRML_INPUT_CONTEXT_KNOWN (1u << 1)
#define CRML_INPUT_FOCUSED       (1u << 2)
#define CRML_INPUT_FRESH         (1u << 3)
#define CRML_INPUT_EMERGENCY     (1u << 4)

// Development builds after Alpha 4.3. Names are NUL-terminated bounded tokens,
// not native key codes. All masks use action-slot bits 0..15. Unknown bits zero.
// A successful snapshot is available even without an installed keyboard source.
typedef struct crml_input_state {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t held;
    uint32_t bound;
    uint32_t duplicate;      // Same key in another slot of this mod.
    uint32_t shared;         // Same key declared by another attached CRML mod.
    uint32_t host_shortcut;  // Potential native F6/F7/F11/Insert use.
    uint64_t binding_revision; // Starts at 1; changes only on this mod's rebind.
    char names[CRML_ACTION_COUNT][CRML_INPUT_KEY_NAME_CAPACITY];
} crml_input_state;

#ifdef __cplusplus
static_assert(sizeof(crml_input_state)==232);
static_assert(offsetof(crml_input_state,binding_revision)==32);
static_assert(offsetof(crml_input_state,names)==40);
#else
_Static_assert(sizeof(crml_input_state)==232, "input state layout");
_Static_assert(offsetof(crml_input_state,binding_revision)==32, "input revision offset");
_Static_assert(offsetof(crml_input_state,names)==40, "input names offset");
#endif
