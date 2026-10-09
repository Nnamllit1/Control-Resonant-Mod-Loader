#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <array>
#include "../sdk/include/crml_abi.h"
#include "../sdk/include/crml_state.h"
#include "../sdk/include/crml_ui.h"
#include "../sdk/include/crml_media.h"
#include "../sdk/include/crml_input.h"
#include "../sdk/include/crml_action_rules.h"

namespace crml {
class ModStorage;
class ModSettings;
class ModFeedback;
class ModTutorials;
class ModDrawing;
class ModLists;
// Compiled runtime version, independent of guest ABI and game fingerprints.
const char* runtime_version() noexcept;
using Log = std::function<void(const std::string&)>;
// Host-owned routing metadata cannot be forged through guest message text.
using GuestLog = std::function<void(std::string_view mod_id, int level, std::string_view text)>;
// Trusted host clock, in milliseconds; guests receive elapsed time relative to
// Runtime construction. Optional injection is used by offline scenario hosts.
using Clock = std::function<uint64_t()>;
// Independent read-only input service; no engine movement/physics ownership.
struct Input {
    virtual bool available() noexcept = 0;
    virtual uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept = 0;
    // Providers without context reporting leave CONTEXT_KNOWN clear. Availability
    // alone never implies focus, a fresh sample, or gameplay/menu ownership.
    virtual uint32_t state_flags() noexcept {return available()?CRML_INPUT_AVAILABLE:0u;}
    // Native providers should copy context and keys together. The default keeps
    // older providers source-compatible; their context may remain unknown.
    virtual void read(const std::array<uint16_t,CRML_ACTION_COUNT>& keys,
                      uint32_t& flags,uint32_t& held) noexcept {
        flags=state_flags();held=(flags&CRML_INPUT_AVAILABLE)?sample(keys):0u;
    }
    virtual ~Input() = default;
};
// Trusted native service. Guests receive only bounded commands, never pointers.
struct Gameplay {
    // Reports installed services, not a ready player/target or an owned lease.
    virtual uint32_t capabilities() const noexcept { return 0; }
    virtual int noclip_poll(uint64_t owner, float speed) noexcept = 0;
    virtual uint32_t input_buttons() noexcept { return 0; }
    virtual int visibility_set(uint64_t, bool) noexcept { return -1; }
    virtual int visibility_poll(uint64_t) noexcept { return -1; }
    virtual int physics_select(uint64_t) noexcept { return -1; }
    virtual int physics_select_near(uint64_t, float, float, float, float) noexcept { return -1; }
    virtual uint64_t physics_target(uint64_t) noexcept { return 0; }
    virtual int physics_apply(uint64_t, uint64_t, float, uint32_t) noexcept { return -1; }
    virtual int physics_status(uint64_t) noexcept { return -1; }
    virtual int physics_restore(uint64_t) noexcept { return -1; }
    virtual int physics_read(uint64_t, uint64_t, crml_physics_state& out) noexcept { out={}; return -1; }
    virtual int player_read(crml_player_state& out) noexcept { out={}; return -1; }
    virtual int navigation_read(crml_navigation_state& out) noexcept { out={}; return -1; }
    virtual int navigation_read_v2(crml_navigation_state_v2& out) noexcept { out={}; return -1; }
    virtual int camera_read(crml_camera_state& out) noexcept { out={}; return -1; }
    virtual int ui_read(crml_ui_state& out) noexcept { out={}; return -1; }
    virtual int ui_activate(uint64_t, uint64_t, uint32_t) noexcept { return -1; }
    virtual int64_t ui_action_submit(uint64_t, uint64_t, uint32_t) noexcept { return -1; }
    virtual int ui_action_status(uint64_t, uint64_t) noexcept { return -1; }
    virtual int ui_present(uint64_t, uint64_t, uint32_t, std::string_view, bool, uint32_t) noexcept { return -1; }
    virtual int media_read(crml_media_state& out) noexcept { out={}; return -1; }
    virtual int media_skip(uint64_t, uint64_t) noexcept { return -1; }
    virtual uint32_t input_motion() noexcept { return 0; }
    virtual int motion_camera(float (&right)[2]) noexcept { right[0]=right[1]=0; return -1; }
    virtual int motion_set(uint64_t, bool, float, float, float) noexcept { return -1; }
    virtual int motion_read(uint64_t,crml_motion_state& out) noexcept { out={};return -1; }
    virtual int visibility_read(uint64_t,crml_visibility_state& out) noexcept { out={};return -1; }
    virtual int action_rule_set(uint64_t,uint32_t,uint32_t) noexcept {return -1;}
    virtual int action_rule_read(uint64_t,crml_action_rule_state& out) noexcept {out={};return -1;}
    virtual void release(uint64_t owner) noexcept = 0;
    virtual ~Gameplay() = default;
};
// All lifecycle calls belong to one host thread. No game pointers cross this API.
class Runtime {
public:
    explicit Runtime(Log log, Gameplay* gameplay = nullptr, Input* input = nullptr, GuestLog guest_log = {}, ModStorage* storage = nullptr, ModSettings* settings = nullptr, ModFeedback* feedback = nullptr, Clock clock = {}, ModTutorials* tutorials = nullptr, ModDrawing* drawing = nullptr, ModLists* lists = nullptr);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    // Optional trusted bootstrap callback receives validated manifest requests
    // before any guest executes. It grants no additional guest permissions.
    void load(const std::filesystem::path& mods, const std::function<void(uint32_t)>& prepare = {});
    void tick(float elapsed_seconds);
    void shutdown();
    // Bounded cumulative host diagnostics; does not reset counters or call a
    // guest. Timing is wall time, independent of an injected simulation clock.
    void report_metrics();
    size_t active() const;
    size_t failures() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
