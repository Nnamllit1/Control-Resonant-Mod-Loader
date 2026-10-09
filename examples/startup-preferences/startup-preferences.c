#include "crml.h"

/* All startup selection, persistence and retry policy belongs to this guest. */
enum { ENABLED, PHOTO, SAVE, PROMPT, MOVIE, HIDE, ERRORS, RESET, SETTING_COUNT };
#define DEFAULTS 95u /* enabled, three prompts, boot movie, error feedback */
#define ALL_PREFERENCES 127u
#define RETRIES 3u
#define LOG(level, text) crml_log(level, text, sizeof(text) - 1)

static const char* keys[SETTING_COUNT] = {
    "enabled", "skip_photosensitivity", "skip_save_warning", "skip_user_interaction",
    "skip_boot_movie", "hide_selected_screens", "show_errors", "reset_preferences"
};
static const char* labels[SETTING_COUNT] = {
    "Enabled", "Skip photosensitivity warning", "Skip save warning", "Skip interaction prompt",
    "Skip boot movie", "Hide selected startup screens", "Show preference errors", "Reset saved preferences"
};
static const char* descriptions[SETTING_COUNT] = {
    "Apply the selected startup preferences.",
    "Request Continue when this warning is ready. Disable to read it at startup.",
    "Request Continue when the save warning is ready. Saving itself is unchanged.",
    "Request Continue at the startup interaction prompt.",
    "Request skipping the known boot movie after two seconds of playback.",
    "Briefly hide selected warning screens while waiting for Continue. Off by default.",
    "Show one preference error at the main menu. No successful-startup messages.",
    "Restore defaults and explicitly replace saved preferences, including an incompatible record."
};
static uint32_t handles[SETTING_COUNT], preferences = DEFAULTS, committed = DEFAULTS;
static uint32_t submitted, save_failures, storage_writable, save_pending, force_save;
static uint32_t settings_ready, error_pending, error_reported;
static uint64_t now_ms, save_after, error_after;
static uint32_t error_waiting;
static uint32_t error_attempts;
static uint64_t error_ticket;

static uint32_t visit_seen, visit_screen, visit_attempts, visit_stop;
static uint64_t ui_generation, action_ticket;
static uint64_t stable_since, action_after, ticket_since, visit_since, presentation_after;
static uint32_t stable_known;
static uint32_t presentation_active;
static uint32_t movie_seen, movie_attempts, movie_accepted;
static uint64_t movie_after;

static int enabled(uint32_t index) { return (preferences & (1u << index)) != 0; }
static void copy_text(char* output, const char* input) {
    while ((*output++ = *input++)) {}
}
static void preference_error(void) {
    if (!error_reported) error_pending = 1;
}
static void record_encode(uint8_t record[12], uint32_t bits) {
    record[0] = 'S'; record[1] = 'P'; record[2] = 'R'; record[3] = 'F';
    record[4] = 1; record[5] = 0; record[6] = 12; record[7] = 0;
    record[8] = (uint8_t)bits; record[9] = 0; record[10] = 0; record[11] = 0;
}
static int record_decode(const uint8_t record[12]) {
    return record[0] == 'S' && record[1] == 'P' && record[2] == 'R' && record[3] == 'F' &&
        record[4] == 1 && record[5] == 0 && record[6] == 12 && record[7] == 0 &&
        !(record[8] & ~ALL_PREFERENCES) && !record[9] && !record[10] && !record[11];
}
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    now_ms = crml_clock_ms();
    save_after = now_ms + 1250;
    const uint32_t caps = crml_capabilities();
    if (caps & CRML_CAP_STORAGE) {
        uint8_t record[12];
        const int result = crml_storage_read(record, sizeof(record));
        if (result == 12 && record_decode(record)) {
            preferences = committed = record[8];
            storage_writable = 1;
        } else if (result == -2) storage_writable = 1;
        else {
            LOG(2, "Saved startup preferences were not recognized or could not be read; preserving the record until explicit reset.");
            preference_error();
        }
    }
    if (!(caps & CRML_CAP_SETTINGS)) {
        LOG(3, "Startup preferences: settings unavailable; startup actions disabled.");
        return;
    }
    /* Settings have their own callback allowance. Eight registrations plus one
       capability query fit without consuming gameplay command slots. */
    for (uint32_t i = 0; i < SETTING_COUNT; ++i) {
        crml_setting_definition definition = {0};
        definition.version = 1;
        definition.kind = CRML_SETTING_BOOL;
        copy_text(definition.key, keys[i]);
        copy_text(definition.label, labels[i]);
        copy_text(definition.description, descriptions[i]);
        definition.initial = i == RESET ? 0 : enabled(i);
        definition.maximum = definition.step = 1;
        const int handle = crml_settings_register(&definition, sizeof(definition));
        if (handle < 1) {
            LOG(3, "Startup preferences: settings registration failed; startup actions disabled.");
            return;
        }
        handles[i] = (uint32_t)handle;
    }
    settings_ready = 1;
}

static void settings_tick(uint32_t caps) {
    crml_setting_value values[SETTING_COUNT];
    if (crml_settings_read(values, sizeof(values)) != SETTING_COUNT) return;
    uint32_t bits = 0;
    for (uint32_t i = 0; i < SETTING_COUNT; ++i) {
        if (values[i].handle != handles[i] || values[i].kind != CRML_SETTING_BOOL ||
            (values[i].value != 0 && values[i].value != 1)) return;
        if (i != RESET && values[i].value == 1) bits |= 1u << i;
    }
    if (values[RESET].value == 1) {
        /* Revision guards preserve intervening UI edits; the next atomic read
           is the authoritative snapshot, including any rejected resets. */
        for (uint32_t i = 0; i < SETTING_COUNT; ++i) {
            const double value = i == RESET ? 0 : ((DEFAULTS >> i) & 1u);
            crml_settings_set(handles[i], value, values[i].revision);
        }
        storage_writable = (caps & CRML_CAP_STORAGE) != 0;
        force_save = 1;
        save_failures = 0;
        save_after = now_ms + 1250;
        if (!storage_writable) preference_error();
        return;
    }
    if (bits != preferences) {
        preferences = bits;
        save_after = now_ms + 1250;
        save_failures = 0;
        if (!storage_writable) {
            LOG(2, "Startup preference changes apply for this session; saving is unavailable or requires explicit reset.");
            preference_error();
        }
    }
}

static void storage_tick(void) {
    if (save_pending) {
        const int status = crml_storage_status();
        if (status == 1) return;
        save_pending = 0;
        if (status == 2) {
            committed = submitted;
            LOG(1, "Startup preferences saved.");
        } else {
            ++save_failures;
            force_save = 1;
            save_after = now_ms + 1250;
            LOG(2, "Startup preferences could not be saved; the previous saved record is unchanged.");
            preference_error();
        }
    }
    if (!storage_writable || save_failures >= RETRIES ||
        (!force_save && preferences == committed) || now_ms < save_after) return;
    uint8_t record[12];
    record_encode(record, preferences);
    const int result = crml_storage_write(record, sizeof(record));
    save_after = now_ms + 1250;
    if (result == 0) {
        submitted = preferences;
        save_pending = 1;
        force_save = 0;
    } else if (result != -4) {
        ++save_failures;
        LOG(2, "Startup preference save request failed.");
        preference_error();
    }
}

static int selected(uint32_t screen) {
    return enabled(ENABLED) &&
        ((screen == CRML_UI_SCREEN_PHOTOSENSITIVITY && enabled(PHOTO)) ||
         (screen == CRML_UI_SCREEN_SAVE_WARNING && enabled(SAVE)) ||
         (screen == CRML_UI_SCREEN_USER_INTERACTION && enabled(PROMPT)));
}
static void receipt_tick(void) {
    if (!action_ticket) return;
    const int status = crml_ui_action_status(action_ticket);
    if (now_ms - ticket_since >= 10000) {
        /* A long callback stall does not replenish the retry window, even if
           the host eventually expires the old request during the same gap. */
        action_ticket = 0;
        visit_stop = 1;
    } else if (status == CRML_UI_ACTION_SKIPPED || status == CRML_UI_ACTION_EXPIRED ||
        status == CRML_UI_ACTION_CANCELLED) {
        action_ticket = 0; /* Explicit evidence that the action was not dispatched. */
        action_after = now_ms + 500;
    } else if (status == CRML_UI_ACTION_DISPATCHED || status == CRML_UI_ACTION_DISPATCH_FAILED ||
               status == -2) {
        action_ticket = 0;
        visit_stop = 1;
        /* A returned trigger, thrown trigger or lost receipt does not prove
           transition completion. Never submit again during this visit. */
    }
    /* QUEUED, DELIVERED, OUTCOME_UNKNOWN and temporary status errors retain the
       receipt. A late definitive report may resolve it; uncertainty is no retry. */
}
static void presentation_tick(const crml_ui_state* ui, uint32_t caps) {
    if (!(caps & CRML_CAP_UI_PRESENTATION)) return;
    const int hide = selected(ui->screen) && enabled(HIDE) && !visit_stop &&
        visit_attempts < RETRIES && now_ms - visit_since < 5000;
    if (now_ms < presentation_after) return;
    presentation_after = now_ms + 250;
    if (hide) {
        if (crml_ui_element_set_hidden(ui->generation, CRML_UI_TARGET_CLASS, "splash", 6, 1, 750) == 0)
            presentation_active = 1;
    } else if (presentation_active) {
        if (crml_ui_element_set_hidden(ui->generation, CRML_UI_TARGET_CLASS, "splash", 6, 0, 0) == 0)
            presentation_active = 0;
    }
}
static uint32_t ui_tick(uint32_t caps) {
    crml_ui_state ui;
    if (!(caps & CRML_CAP_UI_READ) || crml_startup_read_screen(&ui) != 1 ||
        ui.size != sizeof(ui) || ui.version != CRML_UI_STATE_VERSION || !ui.generation ||
        ui.screen == CRML_UI_SCREEN_NONE || ui.screen > CRML_UI_SCREEN_MAIN_MENU) {
        stable_known = 0;
        return CRML_UI_SCREEN_NONE;
    }
    /* Generation includes readiness changes and is not a visit identity. Only
       an observed different screen replenishes this visit's attempt budget.
       An unavailable interval and same-screen page reload do not replenish it. */
    if (!visit_seen || visit_screen != ui.screen) {
        visit_seen = 1;
        visit_screen = ui.screen;
        visit_attempts = visit_stop = 0;
        action_ticket = 0;
        visit_since = now_ms;
        action_after = now_ms + 500;
        stable_known = 0;
        presentation_active = 0;
        presentation_after = now_ms + 250;
    }
    if (!stable_known || ui_generation != ui.generation) {
        ui_generation = ui.generation;
        stable_since = now_ms;
        stable_known = 1;
    }
    if (caps & CRML_CAP_UI_ACTIVATE) receipt_tick();
    presentation_tick(&ui, caps);
    if ((caps & CRML_CAP_UI_ACTIVATE) && selected(ui.screen) && !visit_stop &&
        !action_ticket && visit_attempts < RETRIES && now_ms - stable_since >= 100 && now_ms >= action_after &&
        (ui.actions & CRML_UI_ACTION_MASK_CONTINUE)) {
        ++visit_attempts;
        action_after = now_ms + 500;
        const int64_t result = crml_ui_action_submit(ui.generation, CRML_UI_ACTION_CONTINUE);
        if (result > 0) { action_ticket = (uint64_t)result; ticket_since = now_ms; }
        else if (result != -1 && result != -2 && result != -3) visit_stop = 1;
    }
    return ui.screen;
}
static int boot_asset(const crml_media_state* media) {
    static const char path[] = "textures/videos/uiresources/splash/boot.tex";
    if (media->name_length != sizeof(path) - 1 || media->name[sizeof(path) - 1]) return 0;
    for (uint32_t i = 0; i < sizeof(path) - 1; ++i) {
        const char c = media->name[i] == '\\' ? '/' : media->name[i];
        if (c != path[i]) return 0;
    }
    return 1;
}
static int named_asset(const crml_media_state* media) {
    if (!(media->flags & (CRML_MEDIA_ENGINE_NAME | CRML_MEDIA_MAPPED_NAME)) ||
        !media->name_length || media->name_length >= CRML_MEDIA_NAME_CAPACITY ||
        media->name[media->name_length]) return 0;
    for (uint32_t i = 0; i < media->name_length; ++i)
        if (!media->name[i]) return 0;
    return 1;
}
static void media_tick(uint32_t caps) {
    const uint32_t required = CRML_CAP_MEDIA_READ | CRML_CAP_MEDIA_SKIP;
    if ((caps & required) != required) return;
    crml_media_state media;
    if (crml_startup_read_media(&media) != 1 || media.size != sizeof(media) ||
        media.version != CRML_MEDIA_STATE_VERSION || !media.generation) return;
    /* Missing names and source flags are missing evidence, not another asset.
       The current service reports inactive media as unavailable; that also
       preserves this conservative visit budget. */
    if (!(media.flags & CRML_MEDIA_ACTIVE) || (named_asset(&media) && !boot_asset(&media))) {
        movie_seen = movie_attempts = movie_accepted = 0;
        movie_after = now_ms + 500;
        return;
    }
    if (!named_asset(&media) || !boot_asset(&media)) return;
    if (!movie_seen) { movie_seen = 1; movie_after = now_ms + 500; return; }
    if (!enabled(ENABLED) || !enabled(MOVIE) || movie_accepted || movie_attempts >= RETRIES ||
        now_ms < movie_after || media.elapsed_ms <= 2000 || !(media.flags & CRML_MEDIA_SKIPPABLE) ||
        !(media.flags & (CRML_MEDIA_ENGINE_NAME | CRML_MEDIA_MAPPED_NAME))) return;
    ++movie_attempts;
    movie_after = now_ms + 500;
    /* This older API has no delivery receipt. An accepted request is never
       retried until a proven different/inactive media observation establishes a visit. */
    if (crml_startup_skip_media(media.generation) == 0) movie_accepted = 1;
}
static void feedback_tick(uint32_t caps, uint32_t screen) {
    if (!(caps & CRML_CAP_FEEDBACK)) return;
    if (error_ticket) {
        const int status = crml_feedback_status(error_ticket);
        if (status < 0 || status >= CRML_FEEDBACK_EXPIRED_UNPRESENTED) error_ticket = 0;
    }
    if (!error_pending || error_reported || !enabled(ERRORS) || screen != CRML_UI_SCREEN_MAIN_MENU) {
        error_waiting = 0;
        return;
    }
    if (!error_waiting) { error_waiting = 1; error_after = now_ms + 1250; }
    if (now_ms < error_after || error_attempts >= RETRIES) return;
    error_after = now_ms + 1250;
    ++error_attempts;
    static const char message[] = "Preferences could not be saved or restored. Review Startup Preferences in Options > Mods.";
    const int64_t result = crml_feedback_show(message, sizeof(message) - 1, CRML_FEEDBACK_WARNING, 6000);
    if (result > 0) {
        error_ticket = (uint64_t)result;
        error_reported = 1; /* Queue acceptance is not evidence of presentation. */
        error_pending = 0;
    }
}
void crml_tick(float dt) {
    if (!settings_ready) return;
    (void)dt;
    now_ms = crml_clock_ms();
    /* At most five gameplay observations: clock, capabilities, UI, action status,
       media. At most three commands: UI action, presentation, media skip. */
    const uint32_t caps = crml_capabilities();
    settings_tick(caps);
    storage_tick();
    media_tick(caps);
    const uint32_t screen = ui_tick(caps);
    feedback_tick(caps, screen);
}
void crml_shutdown(void) {
    /* Accepted writes are owned by the host worker. Never spin or submit a new
       save here; unsubmitted edits may be lost on immediate unload. */
    crml_release();
}
