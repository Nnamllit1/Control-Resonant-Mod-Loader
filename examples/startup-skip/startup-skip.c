#include "crml.h"

// All selection and timing policy belongs to this sandboxed mod.
static uint64_t generation;
static uint32_t screen, attempts, observed;
static float stable, retry;
static uint64_t presentation_generation;
static uint32_t presentation_desired;
static float presentation_retry;
static uint64_t media_generation;
static uint32_t media_attempts, media_observed;
static float media_retry;
#define MESSAGE(text) crml_log(1, text, sizeof(text) - 1)

uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) { MESSAGE("Startup skip: ready."); }
void crml_shutdown(void) {}
static void reset_timing(void) { stable = 0; retry = 0; }
static int boot_asset(const crml_media_state* media) {
    static const char path[] = "textures/videos/uiresources/splash/boot.tex";
    if (media->name_length != sizeof(path) - 1 || media->name[sizeof(path) - 1] != 0) return 0;
    for (uint32_t i = 0; i < sizeof(path) - 1; ++i) {
        const char c = media->name[i] == '\\' ? '/' : media->name[i];
        if (c != path[i]) return 0;
    }
    return 1;
}
static void media_tick(float dt) {
    crml_media_state media;
    if (crml_startup_read_media(&media) != 1 || media.size != sizeof(media) ||
        media.version != CRML_MEDIA_STATE_VERSION || !media.generation) {
        media_observed = 0;
        media_retry = 0;
        return;
    }
    if (!media_observed || media_generation != media.generation) {
        if (media_generation != media.generation) media_attempts = 0;
        media_generation = media.generation;
        media_observed = 1;
        media_retry = 0;
        return;
    }
    media_retry += dt;
    if (media_retry > 0.25f) media_retry = 0.25f;
    if (media_attempts >= 10 || media_retry < 0.25f || media.elapsed_ms <= 2000 ||
        (media.flags & (CRML_MEDIA_ACTIVE | CRML_MEDIA_SKIPPABLE)) != (CRML_MEDIA_ACTIVE | CRML_MEDIA_SKIPPABLE) ||
        !(media.flags & (CRML_MEDIA_ENGINE_NAME | CRML_MEDIA_MAPPED_NAME)) || !boot_asset(&media)) return;
    ++media_attempts;
    media_retry = 0;
    if (crml_startup_skip_media(media_generation) == 0) MESSAGE("Startup skip: media skip requested.");
}
static void presentation_tick(const crml_ui_state* ui, float dt) {
    const uint32_t eligible = ui->screen == CRML_UI_SCREEN_PHOTOSENSITIVITY ||
        ui->screen == CRML_UI_SCREEN_SAVE_WARNING || ui->screen == CRML_UI_SCREEN_USER_INTERACTION;
    const uint32_t changed = presentation_generation != ui->generation;
    presentation_generation = ui->generation;
    presentation_retry += dt;
    if (presentation_retry > 0.25f) presentation_retry = 0.25f;
    if (eligible) {
        if (!presentation_desired || changed || presentation_retry >= 0.25f) {
            presentation_desired = 1;
            presentation_retry = 0;
            crml_ui_element_set_hidden(ui->generation, CRML_UI_TARGET_CLASS, "splash", 6, 1, 750);
        }
    } else if (presentation_desired && (changed || presentation_retry >= 0.25f)) {
        presentation_retry = 0;
        if (crml_ui_element_set_hidden(ui->generation, CRML_UI_TARGET_CLASS, "splash", 6, 0, 0) == 0)
            presentation_desired = 0;
    }
}
void crml_tick(float dt) {
    // The media path runs even when the native UI view is not ready yet.
    media_tick(dt);
    crml_ui_state ui;
    if (crml_startup_read_screen(&ui) != 1 || ui.size != sizeof(ui) ||
        ui.version != CRML_UI_STATE_VERSION || !ui.generation) {
        // An unavailable interval breaks the stability window, but does not
        // replenish this generation's attempt budget.
        reset_timing();
        observed = 0;
        return;
    }
    // Presentation is independent of Continue availability and retry exhaustion.
    presentation_tick(&ui, dt);
    if (!observed || generation != ui.generation || screen != ui.screen) {
        if (generation != ui.generation || screen != ui.screen) attempts = 0;
        generation = ui.generation;
        screen = ui.screen;
        observed = 1;
        reset_timing();
        return;
    }
    stable += dt;
    retry += dt;
    if (stable > 0.1f) stable = 0.1f;
    if (retry > 0.25f) retry = 0.25f;
    if (attempts >= 10 || stable < 0.1f || retry < 0.25f ||
        !(ui.actions & CRML_UI_ACTION_MASK_CONTINUE)) return;
    if (screen != CRML_UI_SCREEN_PHOTOSENSITIVITY &&
        screen != CRML_UI_SCREEN_SAVE_WARNING &&
        screen != CRML_UI_SCREEN_USER_INTERACTION) return;
    ++attempts;
    retry = 0;
    if (crml_ui_activate(generation, CRML_UI_ACTION_CONTINUE) == 0)
        MESSAGE("Startup skip: continue requested.");
}
