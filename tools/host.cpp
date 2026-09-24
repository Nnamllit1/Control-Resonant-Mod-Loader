#include "runtime.h"
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: crml_host <mods-directory> [ticks=1]\n";
        return 2;
    }
    try {
        int ticks = 1;
        if (argc == 3) {
            size_t consumed = 0;
            ticks = std::stoi(argv[2], &consumed);
            if (consumed != wcslen(argv[2]) || ticks < 0 || ticks > 10000) throw std::runtime_error("Ticks must be 0..10000");
        }
        crml::Runtime runtime([](const std::string& text) { std::cout << text << '\n'; });
        runtime.load(argv[1]);
        for (int i = 0; i < ticks; ++i) runtime.tick(0.1f);
        std::cout << "Active: " << runtime.active() << "; failures: " << runtime.failures() << '\n';
        runtime.shutdown();
        return runtime.failures() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
