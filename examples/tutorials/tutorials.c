#include "crml.h"
/* Existing game resource; no game assets bundled. Layout belongs to the mod. */
static const crml_tutorial_page hint_page={
    .version=CRML_TUTORIAL_PAGE_VERSION,.kind=CRML_TUTORIAL_HINT,.duration_ms=6000,
    .image={CRML_TUTORIAL_IMAGE_ABOVE,CRML_TUTORIAL_ALIGN_CENTER,80,12,2},
    .title="Native mod tutorial",
    .body="This hint fades after a few seconds. The image above is separate from this text.",
    .image_url="coui://base/textures/uiresources/UI/streamed/default_save_menu_header.png"
};
static const crml_tutorial_page panel_page={
    .version=CRML_TUTORIAL_PAGE_VERSION,.kind=CRML_TUTORIAL_PANEL,
    .image={CRML_TUTORIAL_IMAGE_BELOW,CRML_TUTORIAL_ALIGN_CENTER,80,18,2},
    .title="Native mod tutorial",
    .body="Use Continue to return to the game. The image below is separate from this text.",
    .image_url="coui://base/textures/uiresources/UI/streamed/default_save_menu_header.png"
};
static const crml_tutorial_page prompt_page={
    .version=CRML_TUTORIAL_PAGE_VERSION_OPTIONS,.kind=CRML_TUTORIAL_PROMPT,.duration_ms=6000,
    .title="Native mod prompt",
    .body="Use the game's displayed dismiss action, or wait for the bottom timer to finish.",
    .reserved=CRML_TUTORIAL_OPTION_NATIVE_DISMISS|CRML_TUTORIAL_OPTION_PROGRESS
};
static uint32_t previous;
static int armed;
static int64_t ticket;
static int last_status;
#define LOG(text) crml_log(1,text,sizeof(text)-1)
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void) {return 1;}
CRML_EXPORT("crml_init") void crml_init(void) {LOG("Native tutorials: F9 hint, F10 panel, PageDown prompt, End cancel. Rebind action slots in mod.ini if needed.");}
CRML_EXPORT("crml_tick") void crml_tick(float dt) {
    (void)dt;
    crml_input_state input;
    const int usable=crml_input_read(&input,sizeof(input))==1 &&
       (input.flags&(CRML_INPUT_FOCUSED|CRML_INPUT_FRESH|CRML_INPUT_EMERGENCY))==(CRML_INPUT_FOCUSED|CRML_INPUT_FRESH);
    const uint32_t held=usable?input.held:0;
    const uint32_t pressed=usable && armed?(held&~previous):0;
    previous=held;armed=usable;
    if(ticket>0) {
        const int status=crml_tutorial_status((uint64_t)ticket);
        if(status!=last_status) {
            last_status=status;
            if(status==CRML_TUTORIAL_PRESENTED) LOG("Tutorial presented by native UI.");
            else if(status==CRML_TUTORIAL_DISMISSED) LOG("Tutorial dismissed.");
            else if(status==CRML_TUTORIAL_CANCELLED) LOG("Tutorial cancelled and retired.");
            else if(status==CRML_TUTORIAL_CANCELLING) LOG("Tutorial cancellation pending native cleanup.");
            else if(status==CRML_TUTORIAL_FAILED || status==CRML_TUTORIAL_UNAVAILABLE) LOG("Tutorial unavailable or failed; check the runtime log.");
        }
        // Cancellation is safe on the first fresh sample after focus/staleness.
        // Keep opening edge-triggered so regaining focus cannot create a panel.
        if((held&4u) && (status==CRML_TUTORIAL_QUEUED || status==CRML_TUTORIAL_PRESENTED)) {
            const int result=crml_tutorial_dismiss((uint64_t)ticket);
            if(result==1) LOG("Tutorial cancellation requested.");
            else if(result<0) LOG("Tutorial cancellation request rejected.");
        }
        if(status==CRML_TUTORIAL_QUEUED || status==CRML_TUTORIAL_PRESENTED || status==CRML_TUTORIAL_CANCELLING) return;
    }
    if(pressed&11u) {
        const uint32_t kind=(pressed&2u)?CRML_TUTORIAL_PANEL:
            (pressed&1u)?CRML_TUTORIAL_HINT:CRML_TUTORIAL_PROMPT;
        if(crml_tutorial_available(kind)!=1) {LOG("This native tutorial type is unavailable.");return;}
        const crml_tutorial_page* page=kind==CRML_TUTORIAL_PANEL?&panel_page:
            kind==CRML_TUTORIAL_PROMPT?&prompt_page:&hint_page;
        ticket=crml_tutorial_present(page,sizeof(*page));
        last_status=0;
        if(ticket<0) LOG("Tutorial request rejected or busy.");
    }
}
CRML_EXPORT("crml_shutdown") void crml_shutdown(void) {if(ticket>0)crml_tutorial_dismiss((uint64_t)ticket);}
