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

namespace crml {
using Log = std::function<void(const std::string&)>;
// Independent read-only input service; no engine movement/physics ownership.
struct Input {
    virtual bool available() noexcept = 0;
    virtual uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept = 0;
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
    virtual int camera_read(crml_camera_state& out) noexcept { out={}; return -1; }
    virtual int ui_read(crml_ui_state& out) noexcept { out={}; return -1; }
    virtual int ui_activate(uint64_t, uint64_t, uint32_t) noexcept { return -1; }
    virtual int ui_present(uint64_t, uint64_t, uint32_t, std::string_view, bool, uint32_t) noexcept { return -1; }
    virtual int media_read(crml_media_state& out) noexcept { out={}; return -1; }
    virtual int media_skip(uint64_t, uint64_t) noexcept { return -1; }
    virtual uint32_t input_motion() noexcept { return 0; }
    virtual int motion_camera(float (&right)[2]) noexcept { right[0]=right[1]=0; return -1; }
    virtual int motion_set(uint64_t, bool, float, float, float) noexcept { return -1; }
    virtual void release(uint64_t owner) noexcept = 0;
    virtual ~Gameplay() = default;
};
// All lifecycle calls belong to one host thread. No game pointers cross this API.
class Runtime {
public:
    explicit Runtime(Log log, Gameplay* gameplay = nullptr, Input* input = nullptr);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    // Optional trusted bootstrap callback receives validated manifest requests
    // before any guest executes. It grants no additional guest permissions.
    void load(const std::filesystem::path& mods, const std::function<void(uint32_t)>& prepare = {});
    void tick(float elapsed_seconds);
    void shutdown();
    size_t active() const;
    size_t failures() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
