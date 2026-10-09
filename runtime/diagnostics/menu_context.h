#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace crml::menu_context {
enum class Read : uint32_t {
    ok, arguments, caller, initial_output, output_value, memory,
    in_flight, overlap, stale, unavailable
};
struct Snapshot {
    bool matched_active_context{};
    uint8_t last_match_flag_5c{}; // Raw last-match byte, not a menu subtype.
    uint32_t thread{};
};
// These functions borrow two scalar outputs only on the helper's calling thread.
// prepare runs before the original; inspect runs only after its normal return.
Read prepare(uintptr_t caller,uintptr_t expected_caller,uintptr_t matched,uintptr_t last) noexcept;
Read inspect(uintptr_t matched,uintptr_t last,Snapshot& out) noexcept;
class Cache {
public:
    uint64_t begin() noexcept;
    void finish(uint64_t epoch,Read contract,uintptr_t matched,uintptr_t last,uint64_t now) noexcept;
    bool read(Snapshot& out,Read& status,uint64_t now) noexcept;
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<uint64_t> epoch_{},overlap_epoch_{};
    std::atomic<uint32_t> in_flight_{};
    uint64_t published_epoch_{},sampled_at_{};
    bool published_{};
    Read status_{Read::unavailable};
    Snapshot state_{};
};
// Optional diagnostic only. Negative scans do not establish input ownership.
bool start() noexcept;
bool read(Snapshot& out,Read& status,uint64_t now) noexcept;
const char* name(Read status) noexcept;
#ifdef CRML_MENU_CONTEXT_TESTING
bool test_dispatch();
#endif
}
