#include "crml.h"
// Guest policy: change this mask to CRML_BUTTON_F8 without editing the runtime.
static const uint32_t hide_button = CRML_BUTTON_F7;
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {}
void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t buttons = crml_input_buttons();
    const int32_t should_hide = (buttons & hide_button) != 0;
    // Renew while held; release when up. This does not change collision or AI.
    crml_visibility_set(should_hide);
}
void crml_shutdown(void) { crml_visibility_set(0); }
