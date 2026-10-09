#pragma once
#include <iosfwd>
namespace crml::navigation_observer {
// One bounded read-only capture per process; no public navigation ID API.
bool start() noexcept;
bool active() noexcept;
void stop() noexcept;
void poll(std::ostream&);
#ifdef CRML_NAVIGATION_OBSERVER_TESTING
namespace testing {
bool gate() noexcept;
bool callers() noexcept;
bool callthrough();
void emit(std::ostream&);
}
#endif
}
