#include "crml.h"

static uint64_t observed_revision;
static int pending_feedback;
static int64_t feedback_receipt;
static int text_handle;

uint32_t crml_abi_version(void) { return 1; }

void crml_init(void) {
    const crml_setting_definition enabled = {
        1, CRML_SETTING_BOOL, "enabled", "Enabled", "An example preference; does not change gameplay.",
        0, 0, 1, 1
    };
    const crml_setting_definition speed = {
        1, CRML_SETTING_NUMBER, "speed", "Speed", "Example numeric value with quarter-unit steps.",
        2, 0, 10, 0.25
    };
    const crml_setting_definition count = {
        1, CRML_SETTING_INT, "count", "Count", "Example whole-number value.",
        5, 0, 20, 1
    };
    const crml_text_setting_definition name = {
        1, 64, "marker_name", "Marker name", "Example text preference; does not place a marker.", "Return here"
    };
    text_handle = crml_settings_text_register(&name, sizeof(name));
    if (crml_settings_register(&enabled, sizeof(enabled)) < 1 ||
        crml_settings_register(&speed, sizeof(speed)) < 1 ||
        crml_settings_register(&count, sizeof(count)) < 1 || text_handle < 1) {
        crml_log(3, "Settings registration failed", 28);
    }
}

void crml_tick(float elapsed_seconds) {
    (void)elapsed_seconds;
    crml_setting_value values[4];
    crml_text_setting_value name;
    if (crml_settings_read(values, sizeof(values)) != 4 ||
        crml_settings_text_read((uint32_t)text_handle, &name, sizeof(name)) != 1) return;
    uint64_t revision = values[0].revision + values[1].revision + values[2].revision + values[3].revision;
    if (revision != observed_revision) {
        if (observed_revision) pending_feedback = 1;
        observed_revision = revision;
        crml_log(1, "Settings snapshot changed", 25);
        crml_log(1, name.value, name.length);
        /* Use the copied values here to choose the mod's behavior. Settings
           never activate movement or other gameplay services by themselves. */
    }
    if (feedback_receipt > 0) {
        int status = crml_feedback_status((uint64_t)feedback_receipt);
        if (status == CRML_FEEDBACK_EXPIRED_UNPRESENTED)
            crml_log(2, "Feedback expired without renderer acknowledgement", 49);
        if (status < 0 || status >= CRML_FEEDBACK_EXPIRED_UNPRESENTED) feedback_receipt = 0;
    }
    /* Coalesce edits while another message is visible or rate-limited. */
    if (pending_feedback && (crml_capabilities() & CRML_CAP_FEEDBACK)) {
        int64_t result = crml_feedback_show("Preferences updated.", 20, CRML_FEEDBACK_SUCCESS, 2000);
        if (result > 0) { feedback_receipt = result; pending_feedback = 0; }
        else if (result != -1 && result != -2 && result != -4) pending_feedback = 0;
    }
}
