# Startup skip

A Wasm example that requests Continue on the photosensitivity warning, save warning and user-interaction prompt, and requests skipping the reviewed boot movie. It leaves legal notices, consent screens, calibration, first-time setup and the main menu to the player.

Install the built `startup-skip` folder in `crml/mods`. It requires a runtime with the `ui.read`, `ui.activate`, `media.read`, `media.skip` and `ui.presentation` imports. A runtime without these imports rejects the mod. Unavailable UI or media support disables only the corresponding part of the example.

The mod waits until the same screen generation has been observed for at least 0.1 seconds. It requests only an advertised Continue action, retries no more than once every 0.25 seconds, and makes at most ten attempts per screen generation. A queued request is not confirmation that the screen changed. Native readiness and transition checks remain in force.

Media selection also belongs to the mod. It compares the engine asset path against `textures/videos/uiresources/splash/boot.tex`, allowing either path separator. It requires active, skippable media, a named engine asset reference, and more than two seconds of reported playback. The media adapter may identify the name from the active instance (`ENGINE_NAME`) or from reviewed engine metadata associated with that adapter (`MAPPED_NAME`). Mapped metadata is not evidence of a general movie-enumeration API. Unknown assets are left alone. Retries are limited to ten per generation, at least 0.25 seconds apart.

On the three eligible warning/prompt screens, the mod temporarily hides the uniquely matched native `splash` class. It requests 750 ms presentation leases, renewing every 0.25 seconds even while Continue is unavailable. Other screens request removal of the override; page/screen changes, lease expiry and mod cleanup also restore it. Readiness-only generation changes can retain the lease on the same screen. This changes visibility only, preserving the engine's required initialization and layout. Legal, consent and setup screens are not selected for hiding.

This does not provide instant startup or arbitrary cutscene skipping. Initialization and engine transition checks still apply.

`startup-skip.c` and the hand-authored `startup-skip.wat` implement the same policy. The WAT version builds with `crml_wat.exe`, without a C toolchain. The example reads copied UI state and issues bounded actions; it cannot inject JavaScript or directly edit profiles or save files. The game's ordinary Continue handler may remember that a notice was shown. Saving itself is unchanged.
