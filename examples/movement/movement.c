#include "crml.h"

// All control policy is guest code. The host receives only world velocity.
static uint32_t previous_buttons;
static int32_t enabled;
static float axis(uint32_t buttons, uint32_t positive, uint32_t negative) {
    return (float)((buttons & positive) != 0) - (float)((buttons & negative) != 0);
}
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {}
void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t buttons = crml_input_motion();
    if ((buttons & ~previous_buttons) & CRML_MOTION_F6) enabled = !enabled;
    previous_buttons = buttons;
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
    if (crml_motion_set(enabled, x, y, z) < 0) enabled = 0;
}
void crml_shutdown(void) { crml_motion_set(0, 0, 0, 0); }
