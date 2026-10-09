#include "crml.h"

// All control policy is guest code. The host receives only world velocity.
static uint32_t previous_buttons;
static int32_t enabled;
static int require_toggle_release;
static void report_stop(void) {
    crml_motion_state state;
    if(crml_player_flight_read(&state)!=1 || state.state!=CRML_MOTION_STOPPED) return;
#define REPORT(text) crml_log(1,text,sizeof(text)-1)
    switch(state.stop_reason) {
    case CRML_MOTION_STOP_FOCUS: REPORT("Movement stopped: game lost focus.");break;
    case CRML_MOTION_STOP_ESCAPE: REPORT("Movement stopped: Escape pressed.");break;
    case CRML_MOTION_STOP_LEASE: REPORT("Movement stopped: renewal deadline missed.");break;
    case CRML_MOTION_STOP_STALE_SAMPLE: REPORT("Movement stopped: input or player sample unavailable.");break;
    case CRML_MOTION_STOP_PLAYER_CHANGED:
    case CRML_MOTION_STOP_WORLD_CHANGED:
    case CRML_MOTION_STOP_TELEPORT: REPORT("Movement stopped: player or world changed.");break;
    default: REPORT("Movement stopped by a controller safety check.");break;
    }
#undef REPORT
}
static float axis(uint32_t buttons, uint32_t positive, uint32_t negative) {
    return (float)((buttons & positive) != 0) - (float)((buttons & negative) != 0);
}
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {}
void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t buttons = crml_input_motion();
    crml_input_state input;
    const uint32_t required = CRML_INPUT_AVAILABLE | CRML_INPUT_CONTEXT_KNOWN |
                              CRML_INPUT_FOCUSED | CRML_INPUT_FRESH;
    const int usable = crml_input_read(&input, sizeof(input)) == 1 &&
        input.version == CRML_INPUT_STATE_VERSION &&
        (input.flags & required) == required && !(input.flags & CRML_INPUT_EMERGENCY);
    // A masked zero sample is not evidence that the user released F6. After
    // focus/staleness/emergency cancellation, require an actual usable release.
    const uint32_t toggle = usable && (input.held & 1u) ? CRML_MOTION_F6 : 0u;
    if (!usable || !(input.bound & 1u)) require_toggle_release = 1;
    else if (require_toggle_release) {
        if (!toggle) require_toggle_release = 0;
    } else if (toggle & ~previous_buttons) enabled = !enabled;
    previous_buttons = toggle;
    float x = 0, y = 0, z = 0;
    if (enabled) {
        const float right = axis(buttons, CRML_MOTION_D, CRML_MOTION_A);
        const float forward = axis(buttons, CRML_MOTION_W, CRML_MOTION_S);
        y = axis(buttons, CRML_MOTION_SPACE, CRML_MOTION_CTRL);
        float heading[2];
        if (crml_motion_camera(heading) == 1) {
            x = right * heading[0] - forward * heading[1];
            z = right * heading[1] + forward * heading[0];
        }
        // Compiles to Wasm f32.sqrt; no math library or WASI is needed.
        const float length = __builtin_sqrtf(x*x + y*y + z*z);
        const float speed = (buttons & CRML_MOTION_SHIFT) ? 15.0f : 5.0f;
        const float scale = speed / (length > 1.0f ? length : 1.0f);
        x *= scale; y *= scale; z *= scale;
    }
    const int32_t result = enabled ? crml_player_flight_set_velocity(x, y, z) : crml_player_flight_release();
    if (result < 0) {
        // Read before releasing: explicit cleanup clears this owner's receipt.
        if(enabled) report_stop();
        enabled = 0;
        require_toggle_release = 1;
    }
}
void crml_shutdown(void) { crml_player_flight_release(); }
