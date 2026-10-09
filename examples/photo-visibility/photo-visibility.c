#include "crml.h"

/* This is guest policy only; the existing host owns mesh access and leases. */
enum { PHOTO_ACTION = 1u, MAX_PLAYER_AGE_MS = 500u };
typedef struct preferences { uint8_t enabled, mode, notifications; } preferences;
static preferences prefs = {1, 0, 1}, committed, in_flight;
static uint32_t handles[4];
static int settings_ready, storage_writable, saving, failures, force_save;
static uint64_t now_ms, save_after;
static int armed, previous_down, desired, lease_active, hidden_observed;
static uint64_t player_generation, binding_revision;
static uint32_t previous_conflicts;
static const char* message;
static uint32_t message_severity;
static uint64_t message_until, feedback_after;
static int64_t feedback_receipt;

static uint32_t length(const char* text) {
    uint32_t count = 0;
    while (text[count]) ++count;
    return count;
}
static int equal(preferences a, preferences b) {
    return a.enabled == b.enabled && a.mode == b.mode &&
           a.notifications == b.notifications;
}
static void notice(const char* text, uint32_t severity) {
    crml_log(severity >= CRML_FEEDBACK_WARNING ? 2 : 1, text, length(text));
    if (!prefs.notifications) return;
    if (message && now_ms < message_until && severity < message_severity) return;
    message = text;
    message_severity = severity;
    message_until = now_ms + 4000;
}
static void feedback_tick(void) {
    if (!prefs.notifications || now_ms >= message_until) message = 0;
    if (!message || now_ms < feedback_after) return;
    int64_t result = crml_feedback_show(message, length(message), message_severity, 2000);
    feedback_after = now_ms + 1100;
    if (result > 0) {
        feedback_receipt = result;
        message = 0;
    } else if (result != -1 && result != -2 && result != -4) message = 0;
}

/* Eight explicit bytes: "PVIS", schema 1, enabled, mode, notifications.
   Host storage provides a checksum; no native struct padding is persisted. */
static void load_preferences(void) {
    uint8_t record[8];
    int result = crml_storage_read(record, sizeof(record));
    if (result == -2) storage_writable = 1;
    else if (result == 8 && record[0] == 'P' && record[1] == 'V' &&
             record[2] == 'I' && record[3] == 'S' && record[4] == 1 &&
             record[5] <= 1 && record[6] <= 1 && record[7] <= 1) {
        prefs.enabled = record[5];
        prefs.mode = record[6];
        prefs.notifications = record[7];
        storage_writable = 1;
    } else if (result == -1) {
        notice("Photo preferences are temporary: storage is unavailable.", CRML_FEEDBACK_WARNING);
    } else {
        /* Includes empty, oversized, unknown schema and unreadable records.
           Never replace data we have not successfully understood. */
        notice("Saved photo preferences could not be read. The record is preserved.", CRML_FEEDBACK_WARNING);
    }
    committed = prefs;
}
static void save_failed(void) {
    ++failures;
    save_after = now_ms + 5000;
    notice(failures >= 3 ? "Photo preferences could not be saved. Edit a setting to retry."
                        : "Photo preferences could not be saved. A retry is pending.",
           CRML_FEEDBACK_WARNING);
}
static void storage_tick(void) {
    if (!storage_writable) return;
    if (saving) {
        int status = crml_storage_status();
        if (status == 1) return;
        saving = 0;
        if (status == 2) {
            committed = in_flight;
            if (equal(prefs, committed)) force_save = 0;
            failures = 0;
        } else if (status == -1) {
            storage_writable = 0;
            notice("Photo preferences are temporary: storage became unavailable.", CRML_FEEDBACK_WARNING);
            return;
        } else save_failed();
    }
    if ((!force_save && equal(prefs, committed)) || now_ms < save_after || failures >= 3) return;
    uint8_t record[8] = {'P', 'V', 'I', 'S', 1, prefs.enabled, prefs.mode, prefs.notifications};
    int result = crml_storage_write(record, sizeof(record));
    save_after = now_ms + 1100;
    if (result == 0) { in_flight = prefs; saving = 1; }
    else if (result == -1) {
        storage_writable = 0;
        notice("Photo preferences are temporary: storage became unavailable.", CRML_FEEDBACK_WARNING);
    } else if (result != -4) save_failed();
}
static void cancel(void) {
    if (lease_active) crml_player_mesh_set_hidden(0);
    lease_active = desired = armed = previous_down = hidden_observed = 0;
}
static int read_preferences(void) {
    crml_setting_value values[4];
    if (!settings_ready || crml_settings_read(values, sizeof(values)) != 4) return 0;
    for (int i = 0; i < 4; ++i)
        if (values[i].handle != handles[i]) return 0;
    if (values[3].value != 0) {
        /* Explicit reset is the only path that replaces unknown data.
           The action resets itself and never becomes a persisted preference. */
        const double defaults[4] = {1, 0, 1, 0};
        int reset_ok = 1;
        for (int i = 0; i < 4; ++i)
            if (crml_settings_set(handles[i], defaults[i], 0) < 0) reset_ok = 0;
        cancel();
        if (!reset_ok) {
            notice("Photo preference reset failed. Reload the mod and retry.", CRML_FEEDBACK_WARNING);
            return 0;
        }
        prefs = (preferences){1, 0, 1};
        storage_writable = force_save = 1;
        failures = 0;
        save_after = now_ms + 600;
        notice("Photo preferences reset. Saving is pending.", CRML_FEEDBACK_INFO);
        return 1;
    }
    preferences next = {(uint8_t)values[0].value, (uint8_t)values[1].value,
                        (uint8_t)values[2].value};
    if (equal(next, prefs)) return 1;
    if (next.enabled != prefs.enabled || next.mode != prefs.mode) cancel();
    if (!next.notifications && prefs.notifications) {
        message = 0;
        if (feedback_receipt > 0) crml_feedback_dismiss((uint64_t)feedback_receipt);
        feedback_receipt = 0;
    }
    prefs = next;
    failures = 0;
    /* Coalesce quick edits; the service also enforces its wall-clock limit. */
    if (save_after < now_ms + 600) save_after = now_ms + 600;
    return 1;
}
static void visibility_tick(int settings_ok) {
    crml_input_state input;
    crml_player_state player;
    const uint32_t required = CRML_INPUT_AVAILABLE | CRML_INPUT_CONTEXT_KNOWN |
                              CRML_INPUT_FOCUSED | CRML_INPUT_FRESH;
    int input_ok = crml_input_read(&input, sizeof(input)) == 1;
    int player_ok = crml_player_read(&player, sizeof(player)) == 1;
    if (!settings_ok || !prefs.enabled || !input_ok || !player_ok ||
        input.version != CRML_INPUT_STATE_VERSION || player.version != 1 ||
        (input.flags & required) != required || (input.flags & CRML_INPUT_EMERGENCY) ||
        !(input.bound & PHOTO_ACTION) || !input.binding_revision ||
        !player.generation || player.age_ms > MAX_PLAYER_AGE_MS) {
        cancel();
        player_generation = binding_revision = 0;
        return;
    }
    if (player_generation != player.generation || binding_revision != input.binding_revision) {
        cancel();
        player_generation = player.generation;
        binding_revision = input.binding_revision;
        previous_conflicts = 0;
    }
    if (lease_active) {
        crml_visibility_state visibility;
        if (crml_player_mesh_read(&visibility) != 1 ||
            visibility.version != 1 || visibility.state != CRML_VISIBILITY_ACTIVE) {
            cancel();
            notice("Photo visibility stopped. Release the key and try again.", CRML_FEEDBACK_WARNING);
            return;
        }
        if (!hidden_observed && (visibility.flags & CRML_VISIBILITY_OBSERVED_HIDDEN)) {
            hidden_observed = 1;
            notice("Player mesh hidden.", CRML_FEEDBACK_INFO);
        }
    }
    uint32_t conflicts = ((input.duplicate & PHOTO_ACTION) ? 1u : 0u) |
                         ((input.shared & PHOTO_ACTION) ? 2u : 0u) |
                         ((input.host_shortcut & PHOTO_ACTION) ? 4u : 0u);
    if (conflicts && conflicts != previous_conflicts)
        notice("Photo key has a binding conflict. Check the binding.", CRML_FEEDBACK_WARNING);
    previous_conflicts = conflicts;
    int down = (input.held & PHOTO_ACTION) != 0;
    /* Startup, context recovery, player replacement, rebinds and refused requests
       all require a usable released sample before a new press is accepted. */
    if (!armed) {
        if (!down) armed = 1;
        previous_down = down;
        return;
    }
    if (prefs.mode) {
        if (down && !previous_down) desired = !desired;
    } else desired = down;
    previous_down = down;
    if (!desired) {
        if (lease_active) {
            crml_player_mesh_set_hidden(0);
            lease_active = hidden_observed = 0;
            notice("Player mesh visibility released.", CRML_FEEDBACK_INFO);
        }
        return;
    }
    int result = crml_player_mesh_set_hidden(1);
    if (result == 1) {
        if (!lease_active) notice("Player mesh hide requested.", CRML_FEEDBACK_INFO);
        lease_active = 1;
    } else {
        cancel();
        notice(result == -2 ? "Photo visibility is busy. Release the key and try again."
                            : "Photo visibility is unavailable. Release the key and try again.",
               CRML_FEEDBACK_WARNING);
    }
}

uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    now_ms = crml_clock_ms();
    load_preferences();
    static crml_setting_definition definitions[4] = {
        {1, CRML_SETTING_BOOL, "enabled", "Enabled", "Allow the photo visibility key.", 1, 0, 1, 1},
        {1, CRML_SETTING_INT, "mode", "Activation mode", "0 = hold the key; 1 = press to toggle.", 0, 0, 1, 1},
        {1, CRML_SETTING_BOOL, "notifications", "Notifications", "Show short photo visibility notices.", 1, 0, 1, 1},
        {1, CRML_SETTING_BOOL, "reset_preferences", "Reset preferences", "Restore defaults and replace this mod's saved record, including unknown data. Resets itself.", 0, 0, 1, 1}
    };
    definitions[0].initial = prefs.enabled;
    definitions[1].initial = prefs.mode;
    definitions[2].initial = prefs.notifications;
    settings_ready = 1;
    for (int i = 0; i < 4; ++i) {
        int handle = crml_settings_register(&definitions[i], sizeof(definitions[i]));
        if (handle < 1) settings_ready = 0;
        else handles[i] = (uint32_t)handle;
    }
    if (!settings_ready)
        notice("Photo visibility is disabled: settings are unavailable.", CRML_FEEDBACK_WARNING);
}
void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    now_ms = crml_clock_ms();
    /* New notices and edits get deadlines from this callback, so a long gap
       expires old work without consuming a newly-created notice or debounce. */
    int settings_ok = read_preferences();
    visibility_tick(settings_ok);
    storage_tick();
    feedback_tick();
}
void crml_shutdown(void) {
    now_ms = crml_clock_ms();
    cancel();
    /* At most one final request; acceptance is not a shutdown flush guarantee. */
    save_after = now_ms;
    storage_tick();
    if (feedback_receipt > 0) crml_feedback_dismiss((uint64_t)feedback_receipt);
}
