#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace crml {
using Log = std::function<void(const std::string&)>;
// Trusted native service. Guests receive only bounded commands, never pointers.
struct Gameplay {
    virtual int noclip_poll(uint64_t owner, float speed) noexcept = 0;
    virtual uint32_t input_buttons() noexcept { return 0; }
    virtual int visibility_set(uint64_t, bool) noexcept { return -1; }
    virtual int visibility_poll(uint64_t) noexcept { return -1; }
    virtual void release(uint64_t owner) noexcept = 0;
    virtual ~Gameplay() = default;
};
// All lifecycle calls belong to one host thread. No game pointers cross this API.
class Runtime {
public:
    explicit Runtime(Log log, Gameplay* gameplay = nullptr);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void load(const std::filesystem::path& mods);
    void tick(float elapsed_seconds);
    void shutdown();
    size_t active() const;
    size_t failures() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
