#include "mod_lists.h"
#include <cstring>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace crml {
namespace {
template<size_t N> bool text_valid(const char (&value)[N],bool required) {
    const auto* end=static_cast<const char*>(std::memchr(value,0,N));
    if(!end || (required && end==value))return false;
    if(end==value)return true;
    wchar_t decoded[N]{};
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,static_cast<int>(end-value),decoded,static_cast<int>(N));
    if(!count)return false;
    for(int i=0;i<count;++i)if(decoded[i]<32 || (decoded[i]>=127 && decoded[i]<=159) || decoded[i]==0x2028 || decoded[i]==0x2029)return false;
    return true;
}
bool valid(const crml_list_page& page) {
    if(page.version!=CRML_LIST_VERSION || page.size!=sizeof(page) || page.row_count>CRML_LIST_ROWS_MAX || !std::isfinite(page.font_scale) || page.font_scale<.75f || page.font_scale>1.5f || !text_valid(page.title,true))return false;
    unsigned selected{};
    for(uint32_t i=0;i<page.row_count;++i) {
        const auto& row=page.rows[i];
        if(!row.id || row.reserved || (row.flags&~(CRML_LIST_ROW_ENABLED|CRML_LIST_ROW_SELECTED)) || !text_valid(row.label,true) || !text_valid(row.detail,false))return false;
        if(row.flags&CRML_LIST_ROW_SELECTED)if(++selected>1)return false;
        for(uint32_t j=0;j<i;++j)if(page.rows[j].id==row.id)return false;
    }
    return true;
}
bool same(const crml_list_page& a,const crml_list_page& b) {
    if(a.row_count!=b.row_count || a.font_scale!=b.font_scale || std::strcmp(a.title,b.title))return false;
    for(uint32_t i=0;i<a.row_count;++i) {
        const auto& x=a.rows[i];const auto& y=b.rows[i];
        if(x.id!=y.id || x.flags!=y.flags || std::strcmp(x.label,y.label) || std::strcmp(x.detail,y.detail))return false;
    }
    return true;
}
}
struct ModLists::Impl {
    struct Owner {Group group{};std::deque<crml_list_event> events;uint64_t last_publish{};bool visible{},published{};};
    std::mutex mutex;Clock clock;std::map<uint64_t,Owner> owners;
    uint64_t revision{},sequence{};bool enabled{};
    explicit Impl(Clock source):clock(source?std::move(source):Clock([]{return GetTickCount64();})){}
    void clear(){for(auto& [id,item]:owners){item.visible=false;item.events.clear();}}
};
ModLists::ModLists(Clock clock):impl_(std::make_unique<Impl>(std::move(clock))){}
ModLists::~ModLists()=default;
bool ModLists::attach(uint64_t owner,std::string_view id,const ModMetadata& metadata) {
    if(!owner || id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=id.npos || !metadata.valid())return false;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(s.owners.size()>=32 || s.owners.contains(owner))return false;
    for(const auto& [key,item]:s.owners)if(item.group.id==id)return false;
    Impl::Owner item;item.group.owner=owner;item.group.id=id;item.group.metadata=metadata;
    s.owners.emplace(owner,std::move(item));return true;
}
void ModLists::detach(uint64_t owner){std::lock_guard lock(impl_->mutex);impl_->owners.erase(owner);}
void ModLists::cancel(uint64_t owner){hide(owner);}
int ModLists::hide(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    const auto found=s.owners.find(owner);if(found==s.owners.end())return -1;
    auto& item=found->second;const bool changed=item.visible || !item.events.empty();
    item.visible=false;item.events.clear();return changed?1:0;
}
void ModLists::clear(){std::lock_guard lock(impl_->mutex);impl_->clear();}
void ModLists::enable_renderer(bool enabled){std::lock_guard lock(impl_->mutex);if(!enabled)impl_->clear();impl_->enabled=enabled;}
bool ModLists::available(){std::lock_guard lock(impl_->mutex);return impl_->enabled;}
int64_t ModLists::publish(uint64_t owner,const crml_list_page& page) {
    if(!valid(page))return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    const auto found=s.owners.find(owner);if(!s.enabled || found==s.owners.end())return -1;
    auto& item=found->second;const auto now=s.clock();
    if(item.published && (now<item.last_publish || now-item.last_publish<100))return -4;
    const bool changed=!item.visible || !same(item.group.page,page);
    if(changed && s.revision==INT64_MAX)return -5;
    item.group.page=page;if(changed)item.group.revision=++s.revision;
    item.visible=true;item.published=true;item.last_publish=now;
    return static_cast<int64_t>(item.group.revision);
}
int ModLists::next(uint64_t owner,crml_list_event& output) {
    output={};auto& s=*impl_;std::lock_guard lock(s.mutex);
    const auto found=s.owners.find(owner);if(!s.enabled || found==s.owners.end())return -1;
    auto& events=found->second.events;if(events.empty())return 0;
    output=events.front();events.pop_front();return 1;
}
int ModLists::activate(uint64_t owner,uint64_t revision,uint64_t row_id) {
    if(!owner || !revision || !row_id)return 400;
    auto& s=*impl_;std::lock_guard lock(s.mutex);if(!s.enabled)return 503;
    const auto found=s.owners.find(owner);if(found==s.owners.end())return 404;
    auto& item=found->second;if(!item.visible || item.group.revision!=revision)return 409;
    const auto& page=item.group.page;const crml_list_row* row=nullptr;
    for(uint32_t i=0;i<page.row_count;++i)if(page.rows[i].id==row_id){row=&page.rows[i];break;}
    if(!row)return 404;if(!(row->flags&CRML_LIST_ROW_ENABLED))return 409;
    if(item.events.size()>=CRML_LIST_EVENTS_MAX)return 429;
    if(s.sequence==UINT64_MAX)return 507;
    // Allocation precedes sequence mutation, preserving state on allocation failure.
    item.events.push_back({CRML_LIST_VERSION,0,s.sequence+1,revision,row_id});++s.sequence;return 200;
}
std::vector<ModLists::Group> ModLists::snapshot() {
    auto& s=*impl_;std::lock_guard lock(s.mutex);std::vector<Group> result;
    if(s.enabled)for(const auto& [id,item]:s.owners)if(item.visible)result.push_back(item.group);
    return result;
}
ModLists& process_lists(){static auto* service=new ModLists;return *service;}
}
