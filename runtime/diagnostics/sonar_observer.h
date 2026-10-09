#pragma once
#include "sonar_projection.h"
#include <cstdint>
#include <iosfwd>
namespace crml::sonar_observer {
using SnapshotCallback=void(*)(const sonar_projection::Snapshot&,uint64_t now_ms) noexcept;
bool start(SnapshotCallback callback) noexcept;
void stop() noexcept;
void report_startup(std::ostream&);
void poll(std::ostream&);
}
