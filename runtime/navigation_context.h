#pragma once
#include <cstdint>

namespace crml::navigation_context {
enum class Status { ok, unavailable, arguments, memory, changed, malformed, mismatch };
struct Bundle { uint64_t value{}; bool valid{}; };
struct SavedTransform { float transform[8]{}; uint64_t bundle{}; bool bundle_valid{}; };
enum class RestoreReason {
    eligible, context_disabled, missing_saved_bundle, missing_current_bundle, bundle_mismatch
};
struct RestoreObservation {
    SavedTransform saved{};
    Bundle current{};
    bool context_disabled{};
    RestoreReason reason{RestoreReason::context_disabled};
};
// Borrow only inside the authenticated scheduler callback. The bundle is a
// requested/selected content identifier, not a campaign or persistent frame ID.
Status inspect_bundle(const void* game_state_pointer, Bundle&) noexcept;
// Save capture view+0x108 holds a pointer to a GameState pointer. The extra
// indirection is specific to this capture view, not inspect_bundle's input.
// The exact save capture view already declares both the transform and GameState
// reads. No lookup, allocation, engine call or pointer survives these functions.
Status inspect_save_source(const void* capture_view, SavedTransform&) noexcept;
Status inspect_save_result(const void* destination, const SavedTransform&) noexcept;
// Restore consumer input only: saved points to a 40-byte transform/bundle
// record; restore_view+0xd8 points to a GameState pointer and +0x210 is its
// context gate. Eligibility does not prove that the engine wrote a transform.
Status inspect_restore_source(const void* saved,const void* restore_view,RestoreObservation&) noexcept;
const char* name(Status) noexcept;
const char* name(RestoreReason) noexcept;
}
