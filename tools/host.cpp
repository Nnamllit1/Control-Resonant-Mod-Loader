#include "runtime.h"
#include "session_log.h"
#include "mod_storage.h"
#include "simulation.h"
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--version") {
        std::cout << crml::runtime_version() << '\n';
        return 0;
    }
    const bool profile=argc>2 && std::wstring_view(argv[argc-1])==L"--profile";
    const int arguments=argc-(profile?1:0);
    if (arguments < 2 || arguments > 3) {
        std::cerr << "Usage: crml_host <mods-directory> [ticks=1 | --simulate] [--profile]\n";
        return 2;
    }
    try {
        if(arguments==3 && std::wstring_view(argv[2])==L"--simulate") return simulate(argv[1],profile);
        int ticks = 1;
        if (arguments == 3) {
            size_t consumed = 0;
            ticks = std::stoi(argv[2], &consumed);
            if (consumed != wcslen(argv[2]) || ticks < 0 || ticks > 10000) throw std::runtime_error("Ticks must be 0..10000");
        }
        crml::SessionLog log(std::cout);
        auto mods_path=std::filesystem::absolute(argv[1]).lexically_normal();
        if(mods_path.filename().empty()) mods_path=mods_path.parent_path();
        crml::ModStorage storage(mods_path.parent_path()/"data");
        if(!storage.available()) log.host("Mod persistence unavailable: data directory is blocked, linked or already in use");
        crml::Runtime runtime([&](const std::string& text) {log.host(text);},nullptr,nullptr,
            [&](std::string_view id,int level,std::string_view text) {log.guest(id,level,text);}, &storage);
        runtime.load(argv[1]);
        for (int i = 0; i < ticks; ++i) runtime.tick(0.1f);
        std::cout << "Active: " << runtime.active() << "; failures: " << runtime.failures() << '\n';
        runtime.shutdown();
        if(profile) runtime.report_metrics();
        return runtime.failures() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
