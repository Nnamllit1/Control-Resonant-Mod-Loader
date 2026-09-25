#pragma once
#include <Windows.h>
#include <cstdint>

namespace crml::probe::input {
using Active = bool(*)() noexcept;
bool owned(unsigned key) noexcept;
bool filter(MSG& message) noexcept;
bool filter(RAWINPUT& event, UINT bytes) noexcept;
void filter(BYTE* state) noexcept;
// Process-local hooks; only calls made by the executable are filtered.
bool start(Active active) noexcept;
void release_held(HWND window) noexcept;
uint64_t consumed() noexcept;
}
