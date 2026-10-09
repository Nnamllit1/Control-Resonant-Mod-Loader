# Action restriction exceptions

`player.action_rules` lets a Wasm mod request exceptions to supported restriction
classes for specific player actions. The mod chooses the actions and when to
renew the request. It does not send input or start an action.

Requires ABI 1 and runtime `0.1.0-alpha.4.4.dev.11` or later. This adapter is
build-specific and fails closed when its engine guards or player context cannot
be established. Area exceptions have been exercised in gameplay. Native
instruction fixtures cover arbitration and competing blockers; this does not
establish complete ability behavior across area transitions.

## Supported scope

| Mask | Action |
| --- | --- |
| `CRML_RULE_ACTION_MELEE` | Melee |
| `CRML_RULE_ACTION_DODGE` | Dodge |
| `CRML_RULE_ACTION_JUMP` | Jump |
| `CRML_RULE_ACTION_PARKOUR` | Parkour |
| `CRML_RULE_ACTION_DASH` | Dash |

The supported restriction class is `CRML_RULE_RESTRICTION_AREA`: an applied
story restriction whose reasons consist only of area triggers. Mixed or unknown
reasons receive no exception. Conversation, elevator, quest and television
reasons, flashback mode and other action interferers retain native behavior.

The adapter skips the area interferer at the action evaluator's arbitration
step. It does not clear the player's aggregate story state, alter story facts,
unlock abilities, change costs or cooldowns, or force an action-start result.
Other engine checks can still reject an action. This interface does not promise
all powers or combat in every location.

Starting with runtime `0.1.0-alpha.4.4.dev.12`, the native story-area warning
check excludes actions covered by a currently qualified exception. Other action
bits and restriction reasons retain their warnings; this is not a global
notification filter.

Airborne input processing has additional story-mode gates outside the current
arbitration exception. Floating and fast-movement transitions across story-area
boundaries are not fully supported; an ability carried into such an area can
behave inconsistently. Ordinary sprinting is a separate native action and is
not added to this mask.

## Requests and lifetime

Declare `capabilities=player.action_rules` in `mod.ini` and include `crml.h`.

```c
/* Renew while the mod's own policy permits it; each call replaces this
   mod's previous action mask. Check the result before reporting success. */
int result = crml_action_rule_set(
    CRML_RULE_ACTION_DODGE | CRML_RULE_ACTION_DASH,
    CRML_RULE_RESTRICTION_AREA);

/* Release only this mod's request. */
crml_action_rule_set(0, 0);
```

Requests expire after 500 milliseconds without renewal and bind to the current
player entity and world lifetime. Expiry stops new exceptions when the native
evaluator next consults the restriction; it does not cancel an already-started
action or forcibly clear the engine's result cache. Renewal explicitly binds a new request to the
current context. Unload and guest failure release the owner's request.

Up to 32 owners can hold requests. Their action masks combine; releasing one
owner does not cancel another owner's request. Expired slots may be reclaimed,
so an old expired request can subsequently read as idle. Engine callbacks never
wait for a contended request lock; contention leaves native restrictions intact.

`action_rule_set` consumes one of the eight gameplay commands per callback.

| Result | Meaning |
| --- | --- |
| `1` | Request accepted; does not establish that an action executed |
| `0` | Owner request released |
| `-1` | Service unavailable |
| `-2` | All owner slots occupied by live requests |
| `-3` | Invalid action/restriction combination |
| `-5` | No current player context |

Unknown bits are rejected. A release uses exactly `(0, 0)`.

## Observing a request

`crml_action_rule_read(&state, sizeof(state))` copies a 32-byte
`crml_action_rule_state`, consuming one observation. Result `1` means a snapshot
was copied; `-1` means unavailable and zeroes the output. Invalid guest memory or
a mismatched size traps before accessing the service.

The snapshot reports supported masks, the owner's requested masks and remaining
lease time. States are `IDLE`, `LEASED`, `EXPIRED` and `CONTEXT_CHANGED` with the
`CRML_RULE_` prefix. `LEASED` describes the request, not a guarantee that an engine
restriction was waived or an ability activated.

The [Area actions example](https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/tree/main/examples/area-actions)
shows guest-controlled per-action settings. Its requests are disabled by default.
