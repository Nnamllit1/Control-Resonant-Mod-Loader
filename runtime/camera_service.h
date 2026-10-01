#pragma once
#include "runtime.h"
#include "diagnostics/camera_observation.h"
#include <Windows.h>
#include <atomic>

namespace crml::camera {
// Copies live state on camera-update threads. Consumers receive a historical
// value, never an engine pointer, entity handle, or permission to write a view.
class SnapshotCache {
public:
    void begin() noexcept;
    void finish(void* context,uint64_t now) noexcept;
    int read(crml_camera_state& out,uint64_t now) noexcept;
#ifdef CRML_CAMERA_SERVICE_TESTING
    void test_generation_limit() noexcept {next_generation_=UINT64_MAX;}
#endif
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<uint64_t> epoch_{};
    std::atomic<uint32_t> in_flight_{};
    uint64_t published_epoch_{}, sampled_at_{}, next_generation_{};
    uintptr_t world_{}, global_{};
    uint64_t entity_{};
    crml_camera_state state_{};
};
class Service final : public Gameplay {
public:
    std::string start();
    bool active() const noexcept {return active_;}
    uint32_t capabilities() const noexcept override {return active_?CRML_CAP_CAMERA_READ:0;}
    int camera_read(crml_camera_state& out) noexcept override;
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    void release(uint64_t) noexcept override {}
private:
    bool active_{};
};
}
