#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include "runtime.h"

namespace crml::probe {
class Recorder : public crml::Gameplay {
public:
    std::string start(const std::filesystem::path& root);
    void poll();
    ~Recorder();
    bool active() const { return output_.is_open() || gameplay_; }
    int noclip_poll(uint64_t owner, float speed) noexcept override;
    void release(uint64_t owner) noexcept override;
private:
    std::ofstream output_;
    std::ofstream entity_output_;
    uint64_t last_entity_report_{};
    unsigned polls_{};
    unsigned reports_{};
    bool gameplay_{};
    void* overlay_{};
};
}
