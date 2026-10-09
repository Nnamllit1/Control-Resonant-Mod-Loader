#pragma once
#include "diagnostics/story_reason_snapshot.h"
#include "diagnostics/player_status_lifecycle.h"
#include "diagnostics/structural_lifecycle.h"
#include <atomic>

namespace crml::action_restriction_observer {
// A retained observation, not an authority to bypass engine rules. Component
// addresses are intentionally absent; consumers must resolve the live entity.
// All mask-1 writes participate, independent of diagnostic sampling budgets.
class AppliedStoryState {
public:
    struct Record {
        uintptr_t world{}; // Opaque lifetime comparison only; never dereferenced.
        uint64_t entity{},teardowns{},structural_unwinds{},status_sequence{},writer_sequence{};
        StoryReasons reasons{};
    };
    uint64_t begin_write() noexcept {
        active_.fetch_add(1);
        return sequence_.fetch_add(1)+1;
    }
    void invalidate() noexcept {sequence_.fetch_add(1);}
    void end_write(uint64_t token,const Record* candidate) noexcept {
        const auto ended=sequence_.fetch_add(1)+1;
        active_.fetch_sub(1);
        // Any overlap, rejected origin, exception or unknown writer leaves the
        // previous record stamped with an older sequence and unusable.
        if(!candidate || ended!=token+1 || active_.load() ||
           !TryAcquireSRWLockExclusive(&lock_)) return;
        record_=*candidate;record_.writer_sequence=ended;
        ReleaseSRWLockExclusive(&lock_);
    }
    bool read(uintptr_t world,uint64_t entity,const StoryReasons& current,
              const structural_lifecycle::Snapshot& structural,
              const player_status_lifecycle::Snapshot& lifecycle,Record& out) noexcept {
        out={};
        const auto first=sequence_.load();
        if(active_.load() || !first || !structural.available || structural.active ||
           !lifecycle.available || lifecycle.active || !TryAcquireSRWLockShared(&lock_)) return false;
        const auto value=record_;
        ReleaseSRWLockShared(&lock_);
        if(sequence_.load()!=first || active_.load() || value.writer_sequence!=first ||
           !world || world!=value.world || !entity || entity!=value.entity ||
           structural.teardowns!=value.teardowns || structural.unwinds!=value.structural_unwinds ||
           lifecycle.sequence!=value.status_sequence || current.counts!=value.reasons.counts ||
           current.active_mask!=value.reasons.active_mask || current.enabled!=value.reasons.enabled) return false;
        out=value;return true;
    }
    // Validate again after the caller's final native reads.
    bool current(const Record& value) const noexcept {
        return value.writer_sequence && !active_.load() && value.writer_sequence==sequence_.load();
    }
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<uint64_t> sequence_{},active_{};
    Record record_{};
};
}
