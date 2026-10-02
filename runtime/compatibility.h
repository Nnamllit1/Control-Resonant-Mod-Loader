#pragma once
#include "engine_profile.h"
#include <cstring>
#include <string_view>
#include <filesystem>
#include <iosfwd>
#include <Windows.h>

namespace crml::compatibility {
inline constexpr wchar_t releases_url[] = L"https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases";
// Published on the bootstrap worker before any engine hook is installed.
inline char approved_sha[65]{};
inline bool reviewed_build=false;
inline bool allowed(std::string_view sha) noexcept {
    return sha.size()==64 && (known_build(sha) || sha==std::string_view(approved_sha));
}
// False means the loader must return without starting any mod or engine hook.
bool authorize(const std::filesystem::path& root,std::ostream& log);

// Unknown executables may not map the expected RVAs at all. A failed read is a
// signature mismatch, never a reason to patch the address or dereference it.
inline bool matches(const void* target,const void* expected,size_t size) noexcept {
    __try {return std::memcmp(target,expected,size)==0;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
// Updated profiles carry their own exact bytes, including relocated branches.
// Unmapped code is refused; unknown builds still use the original checks after
// explicit consent, without gaining reviewed-build-only services.
inline bool matches_code(const void* target,uintptr_t previous,const void* expected,size_t size) noexcept {
    if(engine_profile!=EngineProfile::previous) {
        const auto* entry=mapped_address(previous);
        if(!entry || previous>=0x4000000 || size>entry->prefix.size()) return false;
        expected=entry->prefix.data();
    }
    return target && matches(target,expected,size);
}
enum class Choice { ask, allow, deny };
Choice read_choice(const std::filesystem::path&,std::string_view sha);
bool write_choice(const std::filesystem::path&,std::string_view sha,Choice);
}
