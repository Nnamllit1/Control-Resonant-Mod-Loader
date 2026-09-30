#pragma once
#include <cstring>
#include <string_view>
#include <filesystem>
#include <iosfwd>
#include <Windows.h>

namespace crml::compatibility {
inline constexpr std::string_view tested_sha = "2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0";
// Published on the bootstrap worker before any engine hook is installed.
inline char approved_sha[65]{};
inline bool allowed(std::string_view sha) noexcept {
    return sha.size()==64 && (sha==tested_sha || sha==std::string_view(approved_sha));
}
// False means the loader must return without starting any mod or engine hook.
bool authorize(const std::filesystem::path& root,std::ostream& log);

// Unknown executables may not map the expected RVAs at all. A failed read is a
// signature mismatch, never a reason to patch the address or dereference it.
inline bool matches(const void* target,const void* expected,size_t size) noexcept {
    __try {return std::memcmp(target,expected,size)==0;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
enum class Choice { ask, allow, deny };
Choice read_choice(const std::filesystem::path&,std::string_view sha);
bool write_choice(const std::filesystem::path&,std::string_view sha,Choice);
}
