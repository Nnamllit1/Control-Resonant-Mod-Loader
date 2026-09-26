#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>
namespace crml::physics {
class Session {
public:
    std::string start(const std::filesystem::path&);
    void poll();
    bool active() const noexcept { return active_; }
    ~Session();
private:
    std::ofstream log_;
    void* overlay_{};
    uint64_t started_{}, bytes_{}, last_stats_{};
    unsigned keys_{};
    bool active_{};
};
#ifdef CRML_PHYSICS_SESSION_TESTING
bool test_session_prologues();
#endif
}
