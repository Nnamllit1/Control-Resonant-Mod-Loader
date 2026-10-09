#pragma once
#include <array>
#include <string_view>
#include <cstdint>

namespace crml {
// Named, bounded controls only; guests cannot submit native key codes.
struct ActionKey {std::string_view name;uint16_t code;};
inline constexpr ActionKey action_keys[]{
        {"F1",0x70},{"F2",0x71},{"F3",0x72},{"F4",0x73},{"F5",0x74},{"F6",0x75},
        {"F7",0x76},{"F8",0x77},{"F9",0x78},{"F10",0x79},{"F11",0x7a},{"F12",0x7b},
        {"Insert",0x2d},{"Home",0x24},{"End",0x23},{"PageUp",0x21},{"PageDown",0x22},
        {"A",'A'},{"B",'B'},{"C",'C'},{"D",'D'},{"E",'E'},{"F",'F'},{"G",'G'},
        {"H",'H'},{"I",'I'},{"J",'J'},{"K",'K'},{"L",'L'},{"M",'M'},{"N",'N'},
        {"O",'O'},{"P",'P'},{"Q",'Q'},{"R",'R'},{"S",'S'},{"T",'T'},{"U",'U'},
        {"V",'V'},{"W",'W'},{"X",'X'},{"Y",'Y'},{"Z",'Z'},
        {"0",'0'},{"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},
        {"5",'5'},{"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},
        {"Space",0x20},{"Ctrl",0x11},{"Shift",0x10},
        {"Up",0x26},{"Down",0x28},{"Left",0x25},{"Right",0x27}};
inline uint16_t action_key(std::string_view name) noexcept {
    for(const auto& key:action_keys) if(key.name==name) return key.code;
    return 0;
}
inline std::string_view action_name(uint16_t code) noexcept {
    if(!code) return "None";
    for(const auto& key:action_keys) if(key.code==code) return key.name;
    return {};
}
// Potential native conflicts, not a claim that each service is currently active.
// F6/F7 drive legacy poll imports; F11 restores physics; Insert toggles panels.
inline bool action_host_shortcut(uint16_t code) noexcept {
    return code==0x75 || code==0x76 || code==0x7a || code==0x2d;
}
inline int action_slot(std::string_view field) noexcept {
    if(!field.starts_with("action.")) return -1;
    field.remove_prefix(7);
    if(field.size()==1 && field[0]>='0' && field[0]<='9') return field[0]-'0';
    if(field.size()==2 && field[0]=='1' && field[1]>='0' && field[1]<='5') return 10+field[1]-'0';
    return -1;
}
}
