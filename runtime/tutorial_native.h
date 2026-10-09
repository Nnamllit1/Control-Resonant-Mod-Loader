#pragma once
#include <iosfwd>
namespace crml::tutorial_native {
bool start() noexcept;
void stop() noexcept;
void poll(std::ostream&);
}
