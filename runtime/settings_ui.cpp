#include "settings_ui.h"
#include "mod_lists.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace crml {
namespace {
template<class T> bool number(std::string_view text,T& result) {
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    return !text.empty() && parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
}
void quoted(std::string& out,std::string_view value) {
    out+='"';
    for(char c:value) {if(c=='"' || c=='\\')out+='\\';out+=c;}
    out+='"';
}
void real(std::string& out,double value) {
    char buffer[64]{};
    const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,std::numeric_limits<double>::max_digits10);
    if(result.ec!=std::errc{})throw std::runtime_error("Invalid settings value");
    out.append(buffer,result.ptr);
}
bool unhex(std::string_view input,std::string& output) {
    if(input.empty() || input.front()!='t' || input.size()>1+2*CRML_SETTING_TEXT_MAX || input.size()%2!=1)return false;
    auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=1;i<input.size();i+=2) {
        const auto high=digit(input[i]),low=digit(input[i+1]);if(high<0 || low<0)return false;
        output+=static_cast<char>((high<<4)|low);
    }
    return true;
}
std::string serialize(int status,std::vector<ModSettings::Group> groups,const std::vector<ModLists::Group>& lists) {
    for(const auto& list:lists) {
        if(std::none_of(groups.begin(),groups.end(),[&](const auto& group){return group.owner==list.owner;})) {
            ModSettings::Group group;group.owner=list.owner;group.id=list.id;group.metadata=list.metadata;
            groups.push_back(std::move(group));
        }
    }
    std::string out="{\"result\":"+std::to_string(status)+",\"groups\":[";
    bool first_group=true;
    for(const auto& group:groups) {
        if(!first_group)out+=',';first_group=false;
        out+="{\"owner\":\""+std::to_string(group.owner)+"\",\"id\":";quoted(out,group.id);
        out+=",\"name\":";quoted(out,group.metadata.name.empty()?group.id:group.metadata.name);
        out+=",\"version\":";quoted(out,group.metadata.version);
        out+=",\"author\":";quoted(out,group.metadata.author);out+=",\"items\":[";
        bool first_item=true;
        for(const auto& item:group.items) {
            if(!first_item)out+=',';first_item=false;
            const auto& d=item.definition;const auto& s=item.state;
            out+="{\"handle\":"+std::to_string(s.handle)+",\"kind\":"+std::to_string(s.kind)+",\"key\":";quoted(out,d.key);
            out+=",\"label\":";quoted(out,d.label);out+=",\"description\":";quoted(out,d.description);
            out+=",\"initial\":";real(out,d.initial);out+=",\"minimum\":";real(out,d.minimum);
            out+=",\"maximum\":";real(out,d.maximum);out+=",\"step\":";real(out,d.step);
            out+=",\"value\":";
            if(s.kind==CRML_SETTING_TEXT) {quoted(out,item.text);out+=",\"max_bytes\":"+std::to_string(item.max_bytes);}
            else real(out,s.value);
            out+=",\"revision\":\""+std::to_string(s.revision)+"\"}";
        }
        out+="]";
        const auto list=std::find_if(lists.begin(),lists.end(),[&](const auto& item){return item.owner==group.owner;});
        if(list!=lists.end()) {
            out+=",\"list\":{\"revision\":\""+std::to_string(list->revision)+"\",\"title\":";quoted(out,list->page.title);
            out+=",\"font_scale\":";real(out,list->page.font_scale);out+=",\"rows\":[";
            for(uint32_t i=0;i<list->page.row_count;++i) {
                const auto& row=list->page.rows[i];if(i)out+=',';
                out+="{\"id\":\""+std::to_string(row.id)+"\",\"flags\":"+std::to_string(row.flags)+",\"label\":";
                quoted(out,row.label);out+=",\"detail\":";quoted(out,row.detail);out+='}';
            }
            out+="]}";
        }
        out+="}";
    }
    out+="]}";return out;
}
}
void SettingsUi::enable(bool enabled) {std::lock_guard lock(mutex_);if(!enabled && page_!=UINT64_MAX)++page_;enabled_=enabled;sequence_=0;}
uint64_t SettingsUi::open_page() {
    std::lock_guard lock(mutex_);if(!enabled_ || page_==UINT64_MAX)return 0;
    sequence_=0;return ++page_;
}
int SettingsUi::exchange(std::string_view url,std::string& response) noexcept {
    response.clear();if(!url.starts_with(settings_prefix))return 0;
    try {
        if(url.size()>768)return 400;
        std::array<std::string_view,6> fields{};size_t count{};
        url.remove_prefix(settings_prefix.size());
        while(!url.empty() && count<fields.size()) {
            const auto slash=url.find('/');fields[count++]=url.substr(0,slash);
            if(slash==url.npos){url={};break;}
            url.remove_prefix(slash+1);if(url.empty())return 400;
        }
        if(!url.empty() || (count!=2 && count!=6))return 400;
        uint64_t page{},sequence{},owner{},revision{},row{};uint32_t handle{};double value{};std::string text_value;
        const bool list_action=count==6 && fields[3]=="list";
        const bool text_edit=count==6 && !list_action && fields[5].starts_with('t');
        if(!number(fields[0],page) || !number(fields[1],sequence) || !page || !sequence)return 400;
        if(count==6 && (!number(fields[2],owner) || (!list_action && !number(fields[3],handle)) || !number(fields[4],revision) ||
           !owner || (!list_action && !handle) || !revision))return 400;
        if(list_action && (!number(fields[5],row) || !row))return 400;
        if(count==6 && !list_action && (text_edit?!unhex(fields[5],text_value):(!number(fields[5],value) || !std::isfinite(value))))return 400;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock || !enabled_)return 503;
        if(page!=page_)return 403;
        if(sequence<=sequence_)return 409;
        sequence_=sequence;
        int status=0;
        if(list_action) {
            const auto code=lists_?lists_->activate(owner,revision,row):503;
            status=code==200?1:code==409?-4:code==429?-6:code==404?-2:code==400?-3:-1;
        } else if(count==6)status=text_edit?settings_.set_text(owner,handle,text_value,revision):settings_.set(owner,handle,value,revision);
        response=serialize(status,settings_.snapshot(),lists_?lists_->snapshot():std::vector<ModLists::Group>{});return 200;
    } catch(...) {response.clear();return 500;}
}
SettingsUi& process_settings_ui() {static auto* service=new SettingsUi(process_settings(),&process_lists());return *service;}
}
