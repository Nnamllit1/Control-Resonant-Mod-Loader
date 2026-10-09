#pragma once
#include <cstdint>
#include <iosfwd>

namespace crml::action_restriction_observer {
// Opaque native player identity for host-owned leases. Lifetime advances on
// observed world teardown; it is never a guest pointer or retained component.
struct Identity {uintptr_t world{};uint64_t entity{},lifetime{};};
using AllowedActions=uint64_t(*)(const Identity&,uint64_t now) noexcept;
using RequestPending=bool(*)(uint64_t now) noexcept;
bool start_tracking(AllowedActions policy,RequestPending pending=nullptr) noexcept;
void stop_tracking() noexcept;
bool identity(Identity& out) noexcept;
// Monotonic process-local count of exact native story interferers omitted.
uint64_t skipped() noexcept;
uint64_t warning_adjusted() noexcept;
// One bounded, read-only diagnostic capture on the reviewed October hotfix.
bool start() noexcept;
bool active() noexcept;
void stop() noexcept;
void poll(std::ostream&);
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
namespace testing {
void context_fixture(uintptr_t world,uintptr_t other=0) noexcept;
bool callers() noexcept;
bool world_lookup() noexcept;
bool callthrough();
bool invalid_memory();
bool bounds();
bool reporting();
bool reason_capture();
bool writer_capture();
bool writer_budget();
bool native_adapter();
bool native_positive();
}
#endif
}
