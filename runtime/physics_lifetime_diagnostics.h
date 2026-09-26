#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace crml::physics {
struct LifetimeIdentity {
    uint64_t selection{}, scene{}, entity{}, body{};
};
// Read-only identity watch, independent of the writable selection. Keeps no
// engine pointer and performs no engine access. Hooks never wait for this lock.
class LifetimeDiagnostics {
public:
    bool arm(LifetimeIdentity identity) noexcept {
        if(!TryAcquireSRWLockExclusive(&lock_)) {++missed_;return false;}
        identity_=identity;
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }
    bool take(uint64_t scene,uint64_t body,bool whole_scene,LifetimeIdentity& out) noexcept {
        out={};
        if(!TryAcquireSRWLockExclusive(&lock_)) {++missed_;return false;}
        const bool match=identity_.selection && identity_.scene==scene
            && (whole_scene || identity_.body==body);
        if(match) {out=identity_;identity_={};}
        ReleaseSRWLockExclusive(&lock_);
        return match;
    }
    uint64_t missed() const noexcept {return missed_.load();}
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    LifetimeIdentity identity_{};
    std::atomic<uint64_t> missed_{};
};
}
