#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::structural_lifecycle {
// Diagnostic coverage of reviewed command flush and world teardown only.
// These process-local observations are neither campaign IDs nor permission leases.
struct Snapshot {
    uint64_t sequence{},active{},flushes{},teardowns{},unwinds{};
    bool available{};
};
bool start() noexcept;
void stop() noexcept;
Snapshot snapshot() noexcept;
bool quiet_interval(const Snapshot& before,const Snapshot& after) noexcept;
void report(std::ostream&);
#ifdef CRML_STRUCTURAL_LIFECYCLE_TESTING
namespace testing {
bool callthrough();bool concurrent();bool reporting();
void arm();bool begin_flush();void end_flush();
}
#endif
}
