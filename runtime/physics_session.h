#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>
#include "runtime.h"
namespace crml::physics {
class Session : public Gameplay {
public:
    std::string start(const std::filesystem::path&, bool wasm = false, bool shared = false);
    void poll();
    bool active() const noexcept { return active_; }
    uint32_t capabilities() const noexcept override { return active_ && wasm_ ? CRML_CAP_PHYSICS_DAMPING|CRML_CAP_INPUT_BUTTONS : 0u; }
    int noclip_poll(uint64_t, float) noexcept override { return -1; }
    uint32_t input_buttons() noexcept override;
    int physics_select(uint64_t) noexcept override;
    int physics_select_near(uint64_t, float, float, float, float) noexcept override;
    uint64_t physics_target(uint64_t) noexcept override;
    int physics_read(uint64_t, uint64_t, crml_physics_state&) noexcept override;
    int physics_apply(uint64_t, uint64_t, float, uint32_t) noexcept override;
    int physics_status(uint64_t) noexcept override;
    int physics_restore(uint64_t) noexcept override;
    void release(uint64_t) noexcept override;
    ~Session();
private:
#ifdef CRML_PHYSICS_SESSION_TESTING
    friend bool test_session_prologues();
    friend bool test_session_readback();
#endif
    std::ofstream log_;
    void* overlay_{};
    uint64_t started_{}, bytes_{}, last_stats_{};
    unsigned keys_{};
    bool active_{}, wasm_{};
    bool write_log(const std::string&);
};
#ifdef CRML_PHYSICS_SESSION_TESTING
bool test_session_prologues();
bool test_session_readback();
#endif
}
