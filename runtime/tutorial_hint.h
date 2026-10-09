#pragma once
#include <cstdint>

namespace crml::tutorial {
bool initialize_hint(uintptr_t image) noexcept;
bool hint_available() noexcept;
// Stops admission and asks the next native sync to retire the private hint.
// The process-pinned hook remains installed to forward engine work and drain.
void shutdown_hint() noexcept;
}

#ifdef CRML_TUTORIAL_HINT_TESTING
#include "tutorial_payload.h"
namespace crml {class ModTutorials;}
namespace crml::tutorial {
using HintWorker=void(*)(const void*,void*,const void*,const void*,void*);
using HintInputWorker=void(*)(const void*,const void*,const void*);
void hint_test_initialize(HintWorker,const PayloadNative&,ModTutorials&,HintInputWorker=nullptr);
void hint_test_sync(const void*,void*,void*,uint64_t);
void hint_test_input(const void*,const void*,const void*);
bool hint_test_has_payload();
}
#endif
