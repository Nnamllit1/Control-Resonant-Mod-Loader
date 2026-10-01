#pragma once
#include <cstdint>

namespace crml::controller {
using Move = void(*)(void*,void*,void*,void*,void*,void*);
using Movement = void(*)(Move,void*,void*,void*,void*,void*,void*);
using Observer = void(*)(void*,void*) noexcept;
// Startup worker only. One native trampoline, one movement adapter and one
// post-update observer. Callbacks never retain the borrowed engine arguments.
bool start(uintptr_t image) noexcept;
bool movement(Movement callback) noexcept;
bool observe(Observer callback) noexcept;
#ifdef CRML_CONTROLLER_TESTING
bool test_dispatch();
#endif
}
