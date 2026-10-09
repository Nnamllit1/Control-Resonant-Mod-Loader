#pragma once
#include "map_projection.h"
#include <array>
#include <cstdint>
#include <iosfwd>
namespace crml::map_observer {
using SnapshotCallback=void(*)(const map_projection::District&,const std::array<float,4>&,uint64_t) noexcept;
bool start() noexcept;
bool start(SnapshotCallback callback,bool diagnostics) noexcept;
void report_startup(std::ostream&);
void stop() noexcept;
void poll(std::ostream&);
}
