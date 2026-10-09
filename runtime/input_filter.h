#pragma once
#include <Windows.h>
#include <cstdint>

namespace crml::probe::input {
using Active = bool(*)() noexcept;
bool owned(unsigned key) noexcept;
// Cached high bits from the foreground game's message thread. Never queries
// GetAsyncKeyState, whose shared pressed bit may belong to another mod.
bool down(unsigned key) noexcept;
// True only when the foreground message thread supplied a keyboard sample in
// the last 500 ms. No pressed key is required; freshness is not held state.
bool fresh() noexcept;
// Copies one complete fresh keyboard publication. On contention or stale data,
// returns false and zeroes all 256 bytes; never waits on the message thread.
bool snapshot(BYTE* state,uint64_t now) noexcept;
bool filter(MSG& message) noexcept;
bool filter(RAWINPUT& event, UINT bytes) noexcept;
void filter(BYTE* state) noexcept;
// Process-local hooks; only calls made by the executable are filtered.
// An observer may start first. The first non-null callback becomes the pinned
// suppression owner; repeated starts share it, competing callbacks are refused.
bool start(Active active=nullptr) noexcept;
bool observing() noexcept;
void release_held(HWND window) noexcept;
uint64_t consumed() noexcept;
#ifdef CRML_INPUT_TESTING
namespace testing {
void publish(const BYTE* state,uint64_t tick) noexcept;
bool fresh_at(uint64_t now) noexcept;
}
#endif
}
