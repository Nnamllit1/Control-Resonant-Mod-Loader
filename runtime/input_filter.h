#pragma once
#include <Windows.h>
#include <cstdint>

namespace crml::probe::input {
using Active = bool(*)() noexcept;
bool owned(unsigned key) noexcept;
// Cached high bits from the foreground game's message thread. Never queries
// GetAsyncKeyState, whose shared pressed bit may belong to another mod.
bool down(unsigned key) noexcept;
bool filter(MSG& message) noexcept;
bool filter(RAWINPUT& event, UINT bytes) noexcept;
void filter(BYTE* state) noexcept;
// Process-local hooks; only calls made by the executable are filtered.
bool start(Active active=nullptr) noexcept;
void release_held(HWND window) noexcept;
uint64_t consumed() noexcept;
#ifdef CRML_INPUT_TESTING
namespace testing { void publish(const BYTE* state,uint64_t tick) noexcept; }
#endif
}
