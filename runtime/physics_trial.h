#pragma once
#include <cstdint>

namespace crml::physics {
// Internal experiment state, not a guest ABI. All methods run in the owning
// engine callback. The adapter must validate phase, world incarnation and full
// entity/body identities on EVERY access. No engine address is retained here.
struct Target {
    uint64_t epoch{}, entity{}, body{};
    uint32_t local_index{};
    bool operator==(const Target&) const = default;
};
enum class ReadStatus { ok, unavailable, retired };
struct Reading { ReadStatus status{ReadStatus::unavailable}; float value{}; };
class DampingBackend {
public:
    virtual ~DampingBackend() = default;
    virtual Reading read(const Target&) noexcept = 0;
    // false means uncertain, not "nothing changed". The adapter must compare
    // against expected immediately before writing, in its owning phase.
    virtual bool write(const Target&, float expected, float value) noexcept = 0;
};
enum class TrialResult { idle, invalid, busy, unavailable, retired, unchanged,
    applied, active, restore_pending, restored, refused, conflict };
class DampingTrial {
public:
    TrialResult apply(DampingBackend&, const Target&, float value, uint64_t now,
                      uint64_t duration_ms) noexcept;
    TrialResult poll(DampingBackend&, uint64_t now, bool keep) noexcept;
    bool pending() const noexcept { return state_!=State::idle; }
    const Target& target() const noexcept { return target_; }
private:
    enum class State { idle, active, restoring };
    TrialResult clear(TrialResult) noexcept;
    State state_{State::idle};
    Target target_{};
    float original_{}, applied_{};
    uint64_t deadline_{}, last_time_{};
};
}
