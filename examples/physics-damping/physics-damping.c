#include "crml.h"

// Mod policy belongs here: key choices, value, duration and when to apply.
// The native service only selects/validates the body and executes the request.
enum { SELECT_BUTTON = 1u << 0, APPLY_BUTTON = 1u << 1 };
static const float damping = 8.0f;
static const uint32_t duration_ms = 5000;
static const float search_offset[3] = {0, 0, 0};
static const float search_radius = 2.0f;
static uint32_t previous_buttons;
static int32_t previous_status = -100;
static uint64_t retained_target;
static int sampled_match_logged;

uint32_t crml_abi_version(void) { return 1; }

void crml_init(void) {
    static const char message[] = "Physics: Home select, End damping, F11 restore.";
    crml_log(1, message, sizeof(message) - 1);
}

void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t buttons = crml_input_actions();
    const uint32_t pressed = buttons & ~previous_buttons;
    previous_buttons = buttons;

    // An edge, not a held key, starts a search. Bindings live in mod.ini.
    if (pressed & SELECT_BUTTON) {
        const int32_t result = crml_physics_select_near(search_offset[0], search_offset[1], search_offset[2], search_radius);
        if (result == 0) {
            static const char message[] = "Physics: search queued.";
            crml_log(1, message, sizeof(message) - 1);
        } else {
            static const char message[] = "Physics: search unavailable or busy.";
            crml_log(2, message, sizeof(message) - 1);
        }
    }

    // Selection is asynchronous. A zero token means no usable selection yet.
    if (pressed & APPLY_BUTTON) {
        const uint64_t target = crml_physics_target();
        if (target != 0) {
            const int32_t result = crml_physics_apply(target, damping, duration_ms);
            if (result == 0) {
                retained_target = target;
                sampled_match_logged = 0;
                static const char message[] = "Physics: damping queued.";
                crml_log(1, message, sizeof(message) - 1);
            } else {
                static const char message[] = "Physics: damping request rejected.";
                crml_log(2, message, sizeof(message) - 1);
            }
        } else {
            static const char message[] = "Physics: select a prop first; wait for selection.";
            crml_log(2, message, sizeof(message) - 1);
        }
    }
    // physics_target() is unavailable during an active operation. Keep the
    // issued token and inspect copied values; queue acceptance is not readback.
    if (retained_target != 0) {
        crml_physics_state sample;
        const int32_t result = crml_physics_read(retained_target, &sample, sizeof(sample));
        if (result == -3) {
            retained_target = 0;
        } else if (result == 1 && sample.version == 1 && !sampled_match_logged
                   && (sample.flags & CRML_PHYSICS_STATE_DAMPING)
                   && sample.linear_damping == damping) {
            static const char message[] = "Physics: sampled damping matches request.";
            crml_log(1, message, sizeof(message) - 1);
            sampled_match_logged = 1;
        }
    }
    const int32_t status = crml_physics_status();
    if (status == 0 || status == 6 || status == 7 || status == 8 || status == 9)
        retained_target = 0;
    if (status != previous_status) {
        previous_status = status;
        if (status == 3) {
            static const char message[] = "Physics: prop selected.";
            crml_log(1, message, sizeof(message) - 1);
        } else if (status == 4) {
            static const char message[] = "Physics: damping active.";
            crml_log(1, message, sizeof(message) - 1);
        } else if (status == 0) {
            static const char message[] = "Physics: no selected prop.";
            crml_log(1, message, sizeof(message) - 1);
        }
    }
}

// Like the WAT version, this example omits crml_shutdown. Native owner cleanup
// still requests restoration if the mod traps, fails to load or shuts down.
