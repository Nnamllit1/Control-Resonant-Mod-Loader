#include "crml.h"

// Mod policy belongs here: key choices, value, duration and when to apply.
// The native service only selects/validates the body and executes the request.
enum { SELECT_BUTTON = CRML_BUTTON_F7, APPLY_BUTTON = CRML_BUTTON_F8 };
static const float damping = 8.0f;
static const uint32_t duration_ms = 5000;
static uint32_t previous_buttons;

uint32_t crml_abi_version(void) { return 1; }

void crml_init(void) {
    static const char message[] = "Wasm physics: F7 select, F8 damping, F11 restore.";
    crml_log(1, message, sizeof(message) - 1);
}

void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t buttons = crml_input_buttons();
    const uint32_t pressed = buttons & ~previous_buttons;
    previous_buttons = buttons;

    // An edge, not a held key, starts a search. The native panel shows its result.
    if (pressed & SELECT_BUTTON) {
        (void)crml_physics_select();
    }

    // Selection is asynchronous. A zero token means no usable selection yet.
    if (pressed & APPLY_BUTTON) {
        const uint64_t target = crml_physics_target();
        if (target != 0) {
            (void)crml_physics_apply(target, damping, duration_ms);
        }
    }
}

// Like the WAT version, this example omits crml_shutdown. Native owner cleanup
// still requests restoration if the mod traps, fails to load or shuts down.
