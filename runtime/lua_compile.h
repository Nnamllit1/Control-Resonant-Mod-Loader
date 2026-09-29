#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace crml::engine::lua {
inline constexpr size_t max_source_bytes=1024*1024;
struct Compilation {
    std::vector<unsigned char> bytes;
    const char* error{}; // Fixed category; never source text or an internal path.
    unsigned line{};
    explicit operator bool() const noexcept {return !error && !bytes.empty();}
};
// Worker-thread operation; never accesses the game's VM. Input is copied source,
// not precompiled package data. The compiler version and options are pinned.
Compilation compile_source(const std::string& source);
}
