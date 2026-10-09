#pragma once
#include <filesystem>

// Standalone SDK utility only; never linked into the in-game runtime.
int simulate(const std::filesystem::path& mods,bool profile=false);
