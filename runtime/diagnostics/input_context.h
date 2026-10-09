#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace crml::input_context {
enum class Read : uint32_t { ok, arguments, identity, predicate, changed, memory, unavailable };
struct Snapshot {
    uint32_t raw_flags{}, derived_flags{}, thread{};
    bool connected{}, backend{}, generation_matches{}, source_checked{}, source{}, predicate_enabled{};
};
// Inspect only on the native update thread after its original update returns.
// This copies values and never invokes engine predicates or initializes state.
Read inspect(uintptr_t object,uintptr_t expected,uintptr_t predicate_vtable,Snapshot& out) noexcept;

class Cache {
public:
    void begin() noexcept;
    void finish(uintptr_t object,uintptr_t expected,uintptr_t predicate_vtable,uint64_t now) noexcept;
    bool read(Snapshot& out,Read& status,uint64_t now) noexcept;
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<uint64_t> epoch_{};
    std::atomic<uint32_t> in_flight_{};
    uint64_t published_epoch_{}, sampled_at_{};
    bool published_{};
    Read status_{Read::unavailable};
    Snapshot state_{};
};

// Optional development observation; no suppression or guest permission follows
// from these snapshots. Unsupported builds and conflicting hooks are refused.
bool start() noexcept;
bool read(Snapshot& out,Read& status,uint64_t now) noexcept;
const char* name(Read status) noexcept;
#ifdef CRML_INPUT_CONTEXT_TESTING
bool test_dispatch();
#endif
}
