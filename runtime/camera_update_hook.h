#pragma once
#include <cstdint>

namespace crml::camera_update {
using Update = void(*)(void*,void*,void*);
using Adapter = void(*)(Update,void*,void*,void*);
using Observer = void(*)(void*,bool) noexcept;
// Startup-worker registration only. One shared trampoline; borrowed arguments
// remain on the engine thread. The observer receives enter and normal return.
bool start(uintptr_t image) noexcept;
bool adapt(Adapter callback) noexcept;
bool observe(Observer callback) noexcept;
#ifdef CRML_CAMERA_SERVICE_TESTING
bool test_dispatch();
#endif
}
