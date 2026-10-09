#include "mod_actions.h"
#include "action_bindings.h"
#include <algorithm>
#include <cstring>

namespace crml {
ModActions::Owner* ModActions::find(uint64_t id) noexcept {
    if(id) for(auto& owner:owners_) if(owner.id==id)return &owner;
    return nullptr;
}
const ModActions::Owner* ModActions::find(uint64_t id) const noexcept {
    if(id) for(const auto& owner:owners_) if(owner.id==id)return &owner;
    return nullptr;
}
bool ModActions::attach(uint64_t id,const Keys& keys) noexcept {
    if(!id || find(id))return false;
    for(auto key:keys) if(key>=counts_.size() || action_name(key).empty())return false;
    for(auto& owner:owners_) if(!owner.id) {
        owner={id,1,keys};
        std::array<bool,256> seen{};
        for(auto key:keys) if(key && !seen[key]) {++counts_[key];seen[key]=true;}
        return true;
    }
    return false;
}
void ModActions::detach(uint64_t id) noexcept {
    if(auto* owner=find(id)) {
        std::array<bool,256> seen{};
        for(auto key:owner->keys) if(key && !seen[key]) {--counts_[key];seen[key]=true;}
        *owner={};
    }
}
const ModActions::Keys* ModActions::keys(uint64_t id) const noexcept {
    const auto* owner=find(id);return owner?&owner->keys:nullptr;
}
int ModActions::bind(uint64_t id,uint32_t slot,std::string_view name) noexcept {
    auto* owner=find(id);if(!owner)return -1;
    const auto key=action_key(name);
    if(slot>=CRML_ACTION_COUNT || (!key && name!="None"))return -3;
    const auto old=owner->keys[slot];
    if(old==key)return 0;
    if(owner->revision==UINT64_MAX)return -5;
    const bool had_new=std::find(owner->keys.begin(),owner->keys.end(),key)!=owner->keys.end();
    owner->keys[slot]=key;
    if(old && std::find(owner->keys.begin(),owner->keys.end(),old)==owner->keys.end())--counts_[old];
    if(key && !had_new)++counts_[key];
    ++owner->revision;return 1;
}
int ModActions::read(uint64_t id,crml_input_state& out) const noexcept {
    out={};const auto* owner=find(id);if(!owner)return -1;
    out.size=sizeof(out);out.version=CRML_INPUT_STATE_VERSION;out.binding_revision=owner->revision;
    std::array<uint8_t,256> local{};
    for(auto key:owner->keys) if(key)++local[key];
    for(size_t slot=0;slot<owner->keys.size();++slot) {
        const auto key=owner->keys[slot];const uint32_t bit=1u<<slot;
        const auto name=action_name(key);
        std::memcpy(out.names[slot],name.data(),name.size());
        if(!key)continue;
        out.bound|=bit;
        if(local[key]>1)out.duplicate|=bit;
        if(counts_[key]>1)out.shared|=bit;
        if(action_host_shortcut(key))out.host_shortcut|=bit;
    }
    return 1;
}
}
