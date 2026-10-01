#pragma once
#include <array>
#include <string_view>
#include <cstdint>

namespace crml {
// Named, bounded controls only; guests cannot submit native key codes.
inline uint16_t action_key(std::string_view name) noexcept {
    struct Key {std::string_view name;uint16_t code;};
    constexpr Key keys[]{
        {"F1",0x70},{"F2",0x71},{"F3",0x72},{"F4",0x73},{"F5",0x74},{"F6",0x75},
        {"F7",0x76},{"F8",0x77},{"F9",0x78},{"F10",0x79},{"F11",0x7a},{"F12",0x7b},
        {"Insert",0x2d},{"Home",0x24},{"End",0x23},{"PageUp",0x21},{"PageDown",0x22},
        {"W",'W'},{"A",'A'},{"S",'S'},{"D",'D'},{"Q",'Q'},{"E",'E'},{"R",'R'},
        {"Space",0x20},{"Ctrl",0x11},{"Shift",0x10},
        {"Up",0x26},{"Down",0x28},{"Left",0x25},{"Right",0x27}};
    for(const auto& key:keys) if(key.name==name) return key.code;
    return 0;
}
inline int action_slot(std::string_view field) noexcept {
    if(!field.starts_with("action.")) return -1;
    field.remove_prefix(7);
    if(field.size()==1 && field[0]>='0' && field[0]<='9') return field[0]-'0';
    if(field.size()==2 && field[0]=='1' && field[1]>='0' && field[1]<='5') return 10+field[1]-'0';
    return -1;
}
}
