#pragma once
#include "semver.h"
#include <Windows.h>
#include <string>
#include <string_view>

namespace crml {
// Presentation metadata never selects a path, owner identity or permission.
struct ModMetadata {
    std::string name,version,author;
    static bool text(std::string_view value) {
        if(value.size()>95)return false;
        for(const auto c:value)if(static_cast<unsigned char>(c)<32 || c==127)return false;
        return value.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0)>0;
    }
    bool valid() const {return text(name) && text(author) && (version.empty() || Semver::parse(version).has_value());}
};
}
