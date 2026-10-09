#pragma once
#include "../sdk/include/crml_input.h"
#include <array>
#include <string_view>

namespace crml {
// Bounded host-thread registry. No engine pointers, allocations or OS input.
class ModActions {
public:
    using Keys=std::array<uint16_t,CRML_ACTION_COUNT>;
    bool attach(uint64_t owner,const Keys& keys) noexcept;
    void detach(uint64_t owner) noexcept;
    int bind(uint64_t owner,uint32_t slot,std::string_view name) noexcept;
    int read(uint64_t owner,crml_input_state& out) const noexcept;
    const Keys* keys(uint64_t owner) const noexcept;
private:
    struct Owner {uint64_t id{},revision{};Keys keys{};};
    std::array<Owner,32> owners_{};
    // Counts distinct owners of a key, not slots. Duplicate local bindings do
    // not masquerade as cross-mod conflicts. All accepted codes fit one byte.
    std::array<uint8_t,256> counts_{};
    Owner* find(uint64_t owner) noexcept;
    const Owner* find(uint64_t owner) const noexcept;
};
}
