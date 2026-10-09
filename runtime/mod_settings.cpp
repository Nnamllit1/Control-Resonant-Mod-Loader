#include "mod_settings.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <limits>

namespace crml {
namespace {
static_assert(sizeof(crml_setting_definition)==360);
static_assert(sizeof(crml_setting_value)==24);
static_assert(sizeof(crml_text_setting_definition)==584);
static_assert(sizeof(crml_text_setting_value)==280);
bool valid_text_value(std::string_view value) {
    if(value.size()>CRML_SETTING_TEXT_MAX)return false;
    if(value.empty())return true;
    wchar_t decoded[CRML_SETTING_TEXT_MAX]{};
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),decoded,CRML_SETTING_TEXT_MAX);
    if(count<=0)return false;
    for(int i=0;i<count;++i)if(decoded[i]<32 || (decoded[i]>=127 && decoded[i]<=159) || decoded[i]==0x2028 || decoded[i]==0x2029)return false;
    return true;
}
template<size_t N> bool text(const char (&value)[N],bool required) {
    const auto* end=static_cast<const char*>(std::memchr(value,0,N));
    if(!end || (required && end==value))return false;
    for(auto* p=value;p!=end;++p)if(static_cast<unsigned char>(*p)<32 || *p==127)return false;
    return end==value || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,static_cast<int>(end-value),nullptr,0)>0;
}
bool valid_value(const crml_setting_definition& d,double value) {
    if(!std::isfinite(value) || value<d.minimum || value>d.maximum)return false;
    if(d.kind==CRML_SETTING_BOOL && value!=0 && value!=1)return false;
    if(d.kind==CRML_SETTING_INT && std::floor(value)!=value)return false;
    const auto steps=(value-d.minimum)/d.step;
    const auto nearest=d.minimum+std::round(steps)*d.step;
    const auto tolerance=std::max(d.step*1e-7,2*std::numeric_limits<double>::epsilon()*std::max(std::abs(value),std::abs(d.minimum)));
    return std::isfinite(steps) && std::abs(value-nearest)<=tolerance;
}
bool valid(const crml_setting_definition& d) {
    if(d.version!=1 || d.kind<CRML_SETTING_BOOL || d.kind>CRML_SETTING_NUMBER ||
       !text(d.key,true) || !text(d.label,true) || !text(d.description,false))return false;
    if(std::string_view(d.key).find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string_view::npos)return false;
    if(!std::isfinite(d.minimum) || !std::isfinite(d.maximum) || !std::isfinite(d.step) ||
       d.minimum < -1e9 || d.maximum > 1e9 || d.minimum>=d.maximum || d.step<=0 ||
       d.step>d.maximum-d.minimum || (d.maximum-d.minimum)/d.step>1e6)return false;
    if(d.step<4096*std::numeric_limits<double>::epsilon()*std::max(std::abs(d.minimum),std::abs(d.maximum)))return false;
    if(d.kind==CRML_SETTING_BOOL && (d.minimum!=0 || d.maximum!=1 || d.step!=1))return false;
    if(d.kind==CRML_SETTING_INT && (std::floor(d.minimum)!=d.minimum || std::floor(d.maximum)!=d.maximum || std::floor(d.step)!=d.step))return false;
    return valid_value(d,d.maximum) && valid_value(d,d.initial);
}
}
struct ModSettings::Impl {std::mutex mutex;std::map<uint64_t,Group> groups;};
ModSettings::ModSettings():impl_(std::make_unique<Impl>()){}
ModSettings::~ModSettings()=default;
bool ModSettings::attach(uint64_t owner,std::string_view id,const ModMetadata& metadata) {
    if(!owner || id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=id.npos || !metadata.valid())return false;
    std::lock_guard lock(impl_->mutex);
    if(impl_->groups.size()>=32 || impl_->groups.contains(owner))return false;
    for(const auto& [key,group]:impl_->groups)if(group.id==id)return false;
    impl_->groups.emplace(owner,Group{owner,std::string(id),{},metadata});return true;
}
void ModSettings::detach(uint64_t owner) {std::lock_guard lock(impl_->mutex);impl_->groups.erase(owner);}
int ModSettings::define(uint64_t owner,const crml_setting_definition& definition) {
    if(!valid(definition))return -3;
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    auto& items=found->second.items;
    for(const auto& item:items)if(std::strcmp(item.definition.key,definition.key)==0)return -4;
    if(items.size()>=CRML_SETTINGS_MAX)return -5;
    const auto handle=static_cast<uint32_t>(items.size()+1);
    items.push_back({definition,{handle,definition.kind,definition.initial,1}});return static_cast<int>(handle);
}
int ModSettings::read(uint64_t owner,std::span<crml_setting_value> output) {
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    const auto& items=found->second.items;
    if(output.size()<items.size())return -3;
    for(size_t i=0;i<items.size();++i)output[i]=items[i].state;
    return static_cast<int>(items.size());
}
int ModSettings::set(uint64_t owner,uint32_t handle,double value,uint64_t expected_revision) {
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    auto& items=found->second.items;if(!handle || handle>items.size())return -2;
    auto& item=items[handle-1];
    if(item.state.kind==CRML_SETTING_TEXT || !valid_value(item.definition,value))return -3;
    if(expected_revision && expected_revision!=item.state.revision)return -4;
    if(item.state.value==value)return 0;
    if(item.state.revision==UINT64_MAX)return -5;
    item.state.value=value;++item.state.revision;return 1;
}
int ModSettings::define_text(uint64_t owner,const crml_text_setting_definition& d) {
    if(d.version!=1 || !d.max_bytes || d.max_bytes>CRML_SETTING_TEXT_MAX ||
       !text(d.key,true) || !text(d.label,true) || !text(d.description,false) || !text(d.initial,false) ||
       std::string_view(d.key).find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string_view::npos ||
       std::strlen(d.initial)>d.max_bytes || !valid_text_value(d.initial))return -3;
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    auto& items=found->second.items;
    for(const auto& item:items)if(std::strcmp(item.definition.key,d.key)==0)return -4;
    if(items.size()>=CRML_SETTINGS_MAX)return -5;
    Item item{};item.definition.version=1;item.definition.kind=CRML_SETTING_TEXT;
    std::memcpy(item.definition.key,d.key,sizeof(d.key));
    std::memcpy(item.definition.label,d.label,sizeof(d.label));
    std::memcpy(item.definition.description,d.description,sizeof(d.description));
    item.state={static_cast<uint32_t>(items.size()+1),CRML_SETTING_TEXT,0,1};
    item.max_bytes=d.max_bytes;item.text=d.initial;
    items.push_back(std::move(item));return static_cast<int>(items.size());
}
int ModSettings::read_text(uint64_t owner,uint32_t handle,crml_text_setting_value& output) {
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    const auto& items=found->second.items;if(!handle || handle>items.size())return -2;
    const auto& item=items[handle-1];if(item.state.kind!=CRML_SETTING_TEXT)return -3;
    crml_text_setting_value value{1,handle,item.state.revision,item.max_bytes,static_cast<uint32_t>(item.text.size()),{}};
    std::memcpy(value.value,item.text.data(),item.text.size());output=value;return 1;
}
int ModSettings::set_text(uint64_t owner,uint32_t handle,std::string_view value,uint64_t expected_revision) {
    if(!valid_text_value(value))return -3;
    std::lock_guard lock(impl_->mutex);
    const auto found=impl_->groups.find(owner);if(found==impl_->groups.end())return -1;
    auto& items=found->second.items;if(!handle || handle>items.size())return -2;
    auto& item=items[handle-1];if(item.state.kind!=CRML_SETTING_TEXT || value.size()>item.max_bytes)return -3;
    if(expected_revision && expected_revision!=item.state.revision)return -4;
    if(item.text==value)return 0;
    if(item.state.revision==UINT64_MAX)return -5;
    item.text=value;++item.state.revision;return 1;
}
std::vector<ModSettings::Group> ModSettings::snapshot() {
    std::lock_guard lock(impl_->mutex);std::vector<Group> result;
    for(const auto& [key,group]:impl_->groups)if(!group.items.empty())result.push_back(group);
    return result;
}
ModSettings& process_settings() {static auto* service=new ModSettings;return *service;}
}
