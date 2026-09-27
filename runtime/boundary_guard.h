#pragma once
#include "fall_guard.h"
#include <iosfwd>

namespace crml::probe::boundary {
// Requires the executable fingerprint and recovery observer hooks to be checked.
bool start(uintptr_t image,fall::Active active) noexcept;
bool skip_height(const void* view) noexcept;
void write(std::ostream& out);
void stop() noexcept;
#ifdef CRML_FALL_TRACE_TESTING
namespace testing {
void configure(fall::Active,void*) noexcept;
int invoke(void*,int,int,int);
}
#endif
}
