#pragma once

// Source-level names for existing ABI 1 integer results. Imports still return
// int32_t/int64_t; these names change no signature, layout or runtime requirement.
// Use the prefix for the operation being called, not a universal error mapping.
// Invalid guest memory, arguments or exhausted call budgets can trap instead.

// player_read / camera_read: successful snapshots contain version 1.
#define CRML_PLAYER_READ_COPIED       1
#define CRML_PLAYER_READ_NOT_READY    0
#define CRML_PLAYER_READ_UNAVAILABLE (-1)
#define CRML_CAMERA_READ_COPIED       1
#define CRML_CAMERA_READ_NOT_READY    0
#define CRML_CAMERA_READ_UNAVAILABLE (-1)

// input_read succeeds even without keyboard input; inspect its flags.
#define CRML_INPUT_READ_COPIED        1
#define CRML_INPUT_READ_UNAVAILABLE  (-1)
#define CRML_INPUT_BIND_CHANGED       1
#define CRML_INPUT_BIND_UNCHANGED     0
#define CRML_INPUT_BIND_UNAVAILABLE  (-1)
#define CRML_INPUT_BIND_INVALID      (-3)
#define CRML_INPUT_BIND_EXHAUSTED    (-5)

// noclip_poll describes the legacy toggle; motion_set describes a lease request.
#define CRML_NOCLIP_ON                1
#define CRML_NOCLIP_OFF               0
#define CRML_NOCLIP_UNAVAILABLE      (-1)
#define CRML_NOCLIP_OTHER_OWNER      (-2)
#define CRML_MOTION_SET_ACCEPTED      1
#define CRML_MOTION_SET_RELEASED      0
#define CRML_MOTION_SET_UNAVAILABLE  (-1)
#define CRML_MOTION_SET_OTHER_OWNER  (-2)
#define CRML_MOTION_CAMERA_COPIED      1
#define CRML_MOTION_CAMERA_UNAVAILABLE (-1)

// visibility_set / visibility_poll: renewal is not render acknowledgement.
#define CRML_VISIBILITY_RENEWED       1
#define CRML_VISIBILITY_RELEASED      0
#define CRML_VISIBILITY_UNAVAILABLE  (-1)
#define CRML_VISIBILITY_OTHER_OWNER  (-2)

// physics_select / physics_select_near. Invalid regions trap in the guest import.
#define CRML_PHYSICS_SELECT_ACCEPTED  0
#define CRML_PHYSICS_SELECT_UNAVAILABLE (-1)
#define CRML_PHYSICS_SELECT_BUSY     (-2)
// physics_apply: zero/expired/foreign-with-no-owner tokens return INVALID_TARGET.
#define CRML_PHYSICS_APPLY_ACCEPTED   0
#define CRML_PHYSICS_APPLY_UNAVAILABLE (-1)
#define CRML_PHYSICS_APPLY_BUSY      (-2)
#define CRML_PHYSICS_APPLY_INVALID_TARGET (-3)
// physics_restore: accepted includes already idle; restoration can be deferred.
#define CRML_PHYSICS_RESTORE_ACCEPTED 0
#define CRML_PHYSICS_RESTORE_UNAVAILABLE (-1)
#define CRML_PHYSICS_RESTORE_OTHER_OWNER (-2)
// physics_status: FINISHED need not mean a write occurred; terminal states can
// disappear on cleanup. A failed selection can return directly to IDLE.
#define CRML_PHYSICS_STATUS_IDLE       0
#define CRML_PHYSICS_STATUS_QUEUED     1
#define CRML_PHYSICS_STATUS_SEARCHING  2
#define CRML_PHYSICS_STATUS_SELECTED   3
#define CRML_PHYSICS_STATUS_ACTIVE     4
#define CRML_PHYSICS_STATUS_RESTORING  5
#define CRML_PHYSICS_STATUS_FINISHED   6
#define CRML_PHYSICS_STATUS_RETIRED    7
#define CRML_PHYSICS_STATUS_GAME_CONFLICT 8
#define CRML_PHYSICS_STATUS_REFUSED    9
#define CRML_PHYSICS_STATUS_UNAVAILABLE (-1)
#define CRML_PHYSICS_STATUS_BUSY      (-2)
// physics_read; BUSY includes another owner, pending work and lock contention.
#define CRML_PHYSICS_READ_COPIED       1
#define CRML_PHYSICS_READ_NOT_READY    0
#define CRML_PHYSICS_READ_UNAVAILABLE (-1)
#define CRML_PHYSICS_READ_BUSY        (-2)
#define CRML_PHYSICS_READ_INVALID_TARGET (-3)

// UI/media snapshot imports permit NOT_READY, but their current native adapters
// return UNAVAILABLE for absent/stale observations and lock contention.
#define CRML_UI_READ_COPIED            1
#define CRML_UI_READ_NOT_READY         0
#define CRML_UI_READ_UNAVAILABLE      (-1)
#define CRML_MEDIA_READ_COPIED         1
#define CRML_MEDIA_READ_NOT_READY      0
#define CRML_MEDIA_READ_UNAVAILABLE   (-1)
// Legacy ui_activate collapses refusal, stale state, busy and unavailable to -1.
#define CRML_UI_ACTIVATE_ACCEPTED      0
#define CRML_UI_ACTIVATE_REJECTED     (-1)
// ui_action_submit returns a positive receipt, not a success enum.
#define CRML_UI_ACTION_SUBMIT_UNAVAILABLE (-1)
#define CRML_UI_ACTION_SUBMIT_PENDING (-2)
#define CRML_UI_ACTION_SUBMIT_STALE_OR_UNSUPPORTED (-3)
#define CRML_UI_ACTION_SUBMIT_EXHAUSTED (-5)
// ui_action_status positive values remain CRML_UI_ACTION_* in crml_ui.h.
#define CRML_UI_ACTION_STATUS_UNAVAILABLE (-1)
#define CRML_UI_ACTION_STATUS_UNKNOWN_RECEIPT (-2)
#define CRML_UI_PRESENT_ACCEPTED       0
#define CRML_UI_PRESENT_UNAVAILABLE   (-1)
#define CRML_UI_PRESENT_BUSY          (-2)
// media_skip collapses stale, not skippable, occupied and unavailable to -1.
#define CRML_MEDIA_SKIP_ACCEPTED       0
#define CRML_MEDIA_SKIP_REJECTED      (-1)

// storage_read returns a byte count >=0, including a valid empty record.
#define CRML_STORAGE_READ_UNAVAILABLE (-1)
#define CRML_STORAGE_READ_ABSENT      (-2)
#define CRML_STORAGE_READ_BUFFER_TOO_SMALL (-3)
#define CRML_STORAGE_READ_FAILED      (-5) // I/O, corrupt record or host failure.
#define CRML_STORAGE_WRITE_ACCEPTED    0
#define CRML_STORAGE_WRITE_UNAVAILABLE (-1)
#define CRML_STORAGE_WRITE_BUSY       (-4) // Pending write or rate limit.
#define CRML_STORAGE_WRITE_FAILED     (-5) // Host/resource failure at submission.
#define CRML_STORAGE_STATUS_IDLE       0
#define CRML_STORAGE_STATUS_PENDING    1
#define CRML_STORAGE_STATUS_COMMITTED  2
#define CRML_STORAGE_STATUS_UNAVAILABLE (-1)
#define CRML_STORAGE_STATUS_FAILED    (-5)

// settings_register returns a positive owner-local handle.
#define CRML_SETTINGS_REGISTER_UNAVAILABLE (-1)
#define CRML_SETTINGS_REGISTER_INVALID (-3)
#define CRML_SETTINGS_REGISTER_DUPLICATE_KEY (-4)
#define CRML_SETTINGS_REGISTER_RESOURCE_ERROR (-5) // Full or host failure.
// settings_read returns a count >=0, not a COPIED Boolean.
#define CRML_SETTINGS_READ_UNAVAILABLE (-1)
#define CRML_SETTINGS_READ_BUFFER_TOO_SMALL (-3)
#define CRML_SETTINGS_READ_FAILED     (-5) // Host failure.
#define CRML_SETTINGS_SET_CHANGED      1
#define CRML_SETTINGS_SET_UNCHANGED    0
#define CRML_SETTINGS_SET_UNAVAILABLE (-1)
#define CRML_SETTINGS_SET_UNKNOWN_HANDLE (-2)
#define CRML_SETTINGS_SET_INVALID     (-3)
#define CRML_SETTINGS_SET_STALE_REVISION (-4)
#define CRML_SETTINGS_SET_RESOURCE_ERROR (-5) // Exhausted revision or host failure.

// feedback_show returns a positive receipt, not a presentation status.
#define CRML_FEEDBACK_SHOW_UNAVAILABLE (-1)
#define CRML_FEEDBACK_SHOW_BUSY       (-2) // Active message or global capacity.
#define CRML_FEEDBACK_SHOW_INVALID    (-3)
#define CRML_FEEDBACK_SHOW_RATE_LIMITED (-4)
#define CRML_FEEDBACK_SHOW_FAILED     (-5)
// Positive feedback_status states remain CRML_FEEDBACK_* in crml_feedback.h.
#define CRML_FEEDBACK_STATUS_UNAVAILABLE (-1)
#define CRML_FEEDBACK_STATUS_UNKNOWN_RECEIPT (-2)
#define CRML_FEEDBACK_STATUS_FAILED   (-5)
// feedback_dismiss has different positive values from feedback_status.
#define CRML_FEEDBACK_DISMISS_CHANGED  1
#define CRML_FEEDBACK_DISMISS_ALREADY_TERMINAL 0
#define CRML_FEEDBACK_DISMISS_UNAVAILABLE (-1)
#define CRML_FEEDBACK_DISMISS_UNKNOWN_RECEIPT (-2)
#define CRML_FEEDBACK_DISMISS_FAILED  (-5)
