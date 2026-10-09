#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::player_status_lifecycle {
// Invalidates retained observations around the reviewed Status init/copy
// callbacks. This is deliberately global: an unrelated copy may reject an
// observation, but can never make an old one look fresh. Raw relocation and
// ordinary flag writers are separate paths, not covered by this observer.
struct Snapshot {
    uint64_t sequence{},active{},initializations{},copies{},unwinds{};
    bool available{};
};
bool start() noexcept;
void stop() noexcept;
Snapshot snapshot() noexcept;
bool unchanged(const Snapshot&,const Snapshot&) noexcept;
void report(std::ostream&);
#ifdef CRML_PLAYER_STATUS_LIFECYCLE_TESTING
namespace testing {bool callthrough();bool overlapping();bool reporting();bool installation();void arm();void complete_copy();}
#endif
}
