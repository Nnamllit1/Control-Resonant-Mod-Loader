#pragma once
#include "lua_compile.h"
#include "lua_controller.h"
#include <filesystem>
#include <map>

namespace crml::engine::lua {
struct PackageEvent {std::string id;const char* kind;unsigned line{};};
class SourcePackages {
public:
    using Compiler=Compilation(*)(const std::string&);
    SourcePackages(std::filesystem::path root,ControllerHost& host,Compiler compiler=&compile_source);
    // Worker thread only. A source package is <id>/main.luau; a disabled marker
    // in that folder queues unload. Errors preserve the last accepted revision.
    std::vector<PackageEvent> poll(bool enabled=true);
private:
    struct Record {std::string observed;bool seen{},queued{},retry{};};
    std::filesystem::path root_;
    ControllerHost& host_;
    Compiler compile_;
    std::map<std::string,Record> records_;
    const char* scan_error_{};
};
}
