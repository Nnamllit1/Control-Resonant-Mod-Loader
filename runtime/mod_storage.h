#pragma once
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace crml {
// Installation-scoped storage. The trusted host chooses the root; guests choose
// only bytes. One committed record and one pending write per validated mod ID.
class ModStorage {
public:
    static constexpr size_t limit = 65536;
    explicit ModStorage(const std::filesystem::path& root);
    ~ModStorage(); // drains accepted writes; never called from an engine hook
    ModStorage(const ModStorage&) = delete;
    ModStorage& operator=(const ModStorage&) = delete;
    bool available() const noexcept;
    void attach(std::string_view id);
    int read(std::string_view id, std::span<unsigned char> output);
    int write(std::string_view id, std::span<const unsigned char> bytes);
    int status(std::string_view id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
