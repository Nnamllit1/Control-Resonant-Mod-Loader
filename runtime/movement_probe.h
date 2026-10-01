#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include "runtime.h"

namespace crml::probe {
class Recorder : public crml::Gameplay {
public:
    std::string start(const std::filesystem::path& root, uint32_t services=0);
    void poll();
    ~Recorder();
    bool active() const { return output_.is_open() || gameplay_ || visibility_ || read_; }
    bool guest_motion() const noexcept { return motion_; }
    bool has_overlay() const noexcept { return overlay_!=nullptr; }
    uint32_t capabilities() const noexcept override;
    int player_read(crml_player_state& out) noexcept override;
    int noclip_poll(uint64_t owner, float speed) noexcept override;
    uint32_t input_motion() noexcept override;
    int motion_camera(float (&right)[2]) noexcept override;
    int motion_set(uint64_t,bool,float,float,float) noexcept override;
    uint32_t input_buttons() noexcept override;
    int visibility_set(uint64_t owner, bool hidden) noexcept override;
    int visibility_poll(uint64_t owner) noexcept override;
    void release(uint64_t owner) noexcept override;
private:
    std::ofstream output_;
    std::ofstream entity_output_;
    std::ofstream fall_output_;
    uint64_t last_entity_report_{};
    unsigned polls_{};
    unsigned reports_{};
    bool gameplay_{};
    bool motion_{};
    bool visibility_{};
    bool read_{};
    void* overlay_{};
};
}
