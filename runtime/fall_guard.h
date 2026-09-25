#pragma once
#include <cstdint>

namespace crml::probe::fall {
struct Player { uintptr_t world{}; uint64_t entity{}; };
using Active = Player(*)() noexcept;
bool start(uintptr_t image, Active active) noexcept;
bool available(Player player) noexcept;
bool matches_inactive(const void* view,Player player) noexcept;
bool exclude_target(void* result,const void* query,uint64_t entity,Player player) noexcept;
uint64_t skipped_checks() noexcept;
uint64_t skipped_triggers() noexcept;
}
