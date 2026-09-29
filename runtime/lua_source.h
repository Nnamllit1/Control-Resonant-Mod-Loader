#pragma once
#include "lua_vm.h"
#include <filesystem>

namespace crml::engine::lua::source {
// Worker startup: opt-in must exist before hook setup. This does not load files.
bool prepare(const std::filesystem::path& runtime_root);
bool requested() noexcept;
// Called only after the executable, ABI and both lifetime hooks are checked.
bool attach(Api) noexcept;
bool active() noexcept; // Keeps the worker alive while watching/draining.
bool needs_calls() noexcept;
void tick(Context,uint64_t now);
void poll(uint64_t now);
void stop() noexcept;
}
