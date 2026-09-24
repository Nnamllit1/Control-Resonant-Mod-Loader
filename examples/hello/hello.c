#include "crml.h"
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    const char message[] = "Hello from a sandboxed CONTROL Resonant mod!";
    crml_log(1, message, sizeof(message) - 1);
}
void crml_tick(float elapsed_seconds) { (void)elapsed_seconds; }
void crml_shutdown(void) {}
