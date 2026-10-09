#pragma once
#include <stdint.h>

// Public action bits are independent of engine IDs. Only the reported subset
// is supported; these are permission exceptions, never action requests.
#define CRML_RULE_ACTION_MELEE   (1u << 0)
#define CRML_RULE_ACTION_DODGE   (1u << 1)
#define CRML_RULE_ACTION_JUMP    (1u << 2)
#define CRML_RULE_ACTION_PARKOUR (1u << 3)
#define CRML_RULE_ACTION_DASH    (1u << 4)
#define CRML_RULE_ACTION_ALL     31u
#define CRML_RULE_RESTRICTION_AREA (1u << 0)

#define CRML_RULE_IDLE            0u
#define CRML_RULE_LEASED          1u
#define CRML_RULE_EXPIRED         2u
#define CRML_RULE_CONTEXT_CHANGED 3u
typedef struct crml_action_rule_state {
    uint32_t version;
    uint32_t state;
    uint32_t requested_actions;
    uint32_t restrictions;
    uint32_t remaining_ms;
    uint32_t supported_actions;
    uint32_t supported_restrictions;
    uint32_t reserved;
} crml_action_rule_state;

#ifdef __cplusplus
static_assert(sizeof(crml_action_rule_state)==32);
#else
_Static_assert(sizeof(crml_action_rule_state)==32, "action rule state layout");
#endif
