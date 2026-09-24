#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace crml {
using Log = std::function<void(const std::string&)>;
// All lifecycle calls belong to one host thread. No game pointers cross this API.
class Runtime {
public:
    explicit Runtime(Log log);
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
