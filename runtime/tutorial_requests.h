#pragma once
#include <array>
#include <cstdint>

namespace crml::tutorial {
enum class WithdrawalStatus : uint32_t { ok, scope, identity, invalid, unwritable, partial };
struct Withdrawal {
    WithdrawalStatus status{WithdrawalStatus::invalid};
    uint32_t requests{},completions{},selections{};
    bool active{},selected{};
};
// Internal first stage of retirement, for an identifier whose registration the
// caller owns. The addresses must be resolved in the request dispatch scope.
// Does not deactivate, dismiss, complete, erase, free, or acknowledge UI release.
// On partial failure retain the registered payload and require reconciliation
// or recovery. In-place compaction is not transactional; do not blindly retry.
Withdrawal withdraw_queued(const std::array<uintptr_t,7>& components,uint32_t key) noexcept;
}
