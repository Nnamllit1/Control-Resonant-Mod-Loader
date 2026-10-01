#include "crml.h"
static uint32_t previous;
static int first=1;
#define SAY(text) crml_log(1,text,sizeof(text)-1)
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void) { return 1; }
CRML_EXPORT("crml_init") void crml_init(void) {
    if(crml_capabilities()&CRML_CAP_INPUT_ACTIONS) SAY("Input actions ready");
    else SAY("Input actions unavailable");
}
CRML_EXPORT("crml_tick") void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    const uint32_t current=crml_input_actions();
    const uint32_t pressed=first?0:current&~previous;
    previous=current;first=0;
    if(pressed&1u) SAY("Input action 0 pressed");
    if(pressed&2u) SAY("Input action 1 pressed");
}
CRML_EXPORT("crml_shutdown") void crml_shutdown(void) {}
