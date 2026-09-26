#include "crml.h"
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {}
void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    crml_visibility_set((crml_input_buttons() & CRML_BUTTON_F7) != 0);
}
void crml_shutdown(void) { crml_visibility_set(0); }
