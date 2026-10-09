#include "crml.h"

CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void) { return 1; }

CRML_EXPORT("crml_init") void crml_init(void) {
    const char message[] = "My mod initialized.";
    crml_log(1, message, sizeof(message) - 1);
}

CRML_EXPORT("crml_tick") void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    // Add guest policy here. Declare each required capability in mod.ini.
}

CRML_EXPORT("crml_shutdown") void crml_shutdown(void) {
    // Native leases are also cleaned up by the host after traps or shutdown.
}
