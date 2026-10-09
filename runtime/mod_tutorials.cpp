#include "mod_tutorials.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace crml {
namespace {
bool active(int value) {return value==CRML_TUTORIAL_QUEUED || value==CRML_TUTORIAL_PRESENTED || value==CRML_TUTORIAL_CANCELLING;}
bool text_ok(std::string_view text,size_t limit,bool lines) {
    if(text.empty() || text.size()>limit) return false;
    for(unsigned char ch:text) if((ch<32 && !(lines && (ch=='\n' || ch=='\t'))) || ch==127) return false;
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)>0;
}
bool layout_ok(const crml_tutorial_image_layout& image) {
    return image.position<=CRML_TUTORIAL_IMAGE_BELOW && image.alignment<=CRML_TUTORIAL_ALIGN_RIGHT &&
        image.width_percent<=100 && image.max_height_vh<=32 && image.gap_vh<=4;
}
template<size_t N> bool descriptor_text(const char (&buffer)[N],std::string_view& out) {
    const auto end=std::find(buffer,buffer+N,'\0');
    if(end==buffer+N) return false;
    out={buffer,static_cast<size_t>(end-buffer)};return true;
}
}
struct ModTutorials::Impl {
    struct Entry {Request request;int status{};bool claimed{},detached{};};
    std::mutex mutex;
    std::map<uint64_t,Entry> owners;
    // The engine's missing-localization-key cache has no established periodic
    // eviction. Repeated reloads cannot bypass this process-wide text bound.
    std::set<std::pair<std::string,std::string>> distinct_text;
    std::array<bool,3> enabled{};
    uint64_t next_ticket{1};
    static void cancel(Entry& e) {
        if(active(e.status)) e.status=e.claimed?CRML_TUTORIAL_CANCELLING:CRML_TUTORIAL_CANCELLED;
    }
};
ModTutorials::ModTutorials():impl_(std::make_unique<Impl>()){}
ModTutorials::~ModTutorials()=default;
bool ModTutorials::attach(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    return owner && s.owners.size()<32 && s.owners.try_emplace(owner).second;
}
void ModTutorials::cancel(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto found=s.owners.find(owner);
    if(found!=s.owners.end()) Impl::cancel(found->second);
}
void ModTutorials::detach(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto found=s.owners.find(owner);
    if(found==s.owners.end()) return;
    auto& e=found->second;Impl::cancel(e);e.detached=true;
    if(!active(e.status)) s.owners.erase(found);
}
int64_t ModTutorials::present(uint64_t owner,const crml_tutorial_page& page) {
    std::string_view title,body,url;
    if((page.version!=CRML_TUTORIAL_PAGE_VERSION && page.version!=CRML_TUTORIAL_PAGE_VERSION_OPTIONS) ||
       (page.version==CRML_TUTORIAL_PAGE_VERSION && page.reserved) ||
       !descriptor_text(page.title,title) || !descriptor_text(page.body,body) || !descriptor_text(page.image_url,url)) return -3;
    return show(owner,page.kind,title,body,page.duration_ms,url,page.image,
        page.version==CRML_TUTORIAL_PAGE_VERSION_OPTIONS?page.reserved:UINT32_MAX);
}
int64_t ModTutorials::show(uint64_t owner,uint32_t kind,std::string_view title,std::string_view body,uint32_t duration,
    std::string_view image_url,crml_tutorial_image_layout image,uint32_t options) {
    if(options==UINT32_MAX) options=kind==CRML_TUTORIAL_PROMPT?
        CRML_TUTORIAL_OPTION_NATIVE_DISMISS|CRML_TUTORIAL_OPTION_PROGRESS:0;
    if(kind>CRML_TUTORIAL_PROMPT || (options&~(CRML_TUTORIAL_OPTION_NATIVE_DISMISS|CRML_TUTORIAL_OPTION_PROGRESS)) ||
       (kind!=CRML_TUTORIAL_PROMPT && options) ||
       !text_ok(title,CRML_TUTORIAL_TITLE_MAX,false) || !text_ok(body,CRML_TUTORIAL_BODY_MAX,true) ||
       !layout_ok(image) ||
       (!image_url.empty() && !tutorial_image_url_valid(image_url)) ||
       (kind==CRML_TUTORIAL_PANEL?duration!=0:(duration<1000 || duration>30000))) return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto found=s.owners.find(owner);
    if(found==s.owners.end() || found->second.detached || !s.enabled[kind]) return -1;
    auto& e=found->second;
    if(active(e.status) || std::count_if(s.owners.begin(),s.owners.end(),[](const auto& p){return active(p.second.status);})>=8) return -2;
    if(s.next_ticket>INT64_MAX) return -5;
    // Account for the actual localized string, including layout variants.
    // Explicit default values and zero defaults therefore share one entry.
    std::pair<std::string,std::string> text{title,tutorial_body_markup(body,image_url,image)};
    if(!s.distinct_text.contains(text) && s.distinct_text.size()>=64) return -5;
    Request request{s.next_ticket,owner,kind,duration,std::string(title),std::string(body),std::string(image_url),image,
        static_cast<uint8_t>(options)};
    s.distinct_text.insert(std::move(text));++s.next_ticket;
    e.request=std::move(request);e.status=CRML_TUTORIAL_QUEUED;e.claimed=false;
    return static_cast<int64_t>(e.request.ticket);
}
int ModTutorials::status(uint64_t owner,uint64_t ticket) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto found=s.owners.find(owner);
    if(found==s.owners.end() || found->second.detached) return -1;
    return ticket && found->second.request.ticket==ticket?found->second.status:-2;
}
int ModTutorials::dismiss(uint64_t owner,uint64_t ticket) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto found=s.owners.find(owner);
    if(found==s.owners.end() || found->second.detached) return -1;
    auto& e=found->second;if(!ticket || e.request.ticket!=ticket) return -2;
    if(!active(e.status)) return 0;
    Impl::cancel(e);return 1;
}
bool ModTutorials::available(uint32_t kind) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);return kind<3 && s.enabled[kind];
}
void ModTutorials::enable(uint32_t kind,bool enabled) {
    if(kind>CRML_TUTORIAL_PROMPT) return;
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.enabled[kind]=enabled;
    if(!enabled) for(auto& [owner,e]:s.owners) if(e.request.kind==kind) Impl::cancel(e);
}
bool ModTutorials::take(uint32_t kind,Request& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(kind>CRML_TUTORIAL_PROMPT || !s.enabled[kind]) return false;
    auto selected=s.owners.end();
    for(auto it=s.owners.begin();it!=s.owners.end();++it) {
        auto& e=it->second;
        if(e.request.kind==kind && e.claimed && active(e.status)) return false;
        if(e.request.kind==kind && e.status==CRML_TUTORIAL_QUEUED && !e.claimed && !e.detached &&
           (selected==s.owners.end() || e.request.ticket<selected->second.request.ticket)) selected=it;
    }
    if(selected==s.owners.end()) return false;
    out=selected->second.request;selected->second.claimed=true;return true;
}
bool ModTutorials::take_dynamic(Request& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);
    auto selected=s.owners.end();
    for(auto it=s.owners.begin();it!=s.owners.end();++it) {
        auto& e=it->second;
        if(e.request.kind!=CRML_TUTORIAL_HINT && e.request.kind!=CRML_TUTORIAL_PROMPT) continue;
        if(e.claimed && active(e.status)) return false;
        if(s.enabled[e.request.kind] && e.status==CRML_TUTORIAL_QUEUED && !e.claimed && !e.detached &&
           (selected==s.owners.end() || e.request.ticket<selected->second.request.ticket)) selected=it;
    }
    if(selected==s.owners.end()) return false;
    out=selected->second.request;selected->second.claimed=true;return true;
}
bool ModTutorials::cancelled(uint64_t ticket) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    for(const auto& [owner,e]:s.owners) if(e.request.ticket==ticket)
        return e.detached || e.status==CRML_TUTORIAL_CANCELLING || !active(e.status);
    return true;
}
void ModTutorials::report(uint64_t ticket,int status) {
    if(status!=CRML_TUTORIAL_PRESENTED && status!=CRML_TUTORIAL_DISMISSED && status!=CRML_TUTORIAL_CANCELLED &&
       status!=CRML_TUTORIAL_UNAVAILABLE && status!=CRML_TUTORIAL_FAILED) return;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    for(auto it=s.owners.begin();it!=s.owners.end();++it) {
        auto& e=it->second;if(!ticket || e.request.ticket!=ticket || !e.claimed || !active(e.status)) continue;
        if(status==CRML_TUTORIAL_PRESENTED && e.status==CRML_TUTORIAL_CANCELLING) return;
        e.status=status;
        if(e.detached && !active(status)) s.owners.erase(it);
        return;
    }
}
ModTutorials& process_tutorials() {static ModTutorials service;return service;}
bool tutorial_image_url_valid(std::string_view url) {
    constexpr std::string_view prefix="coui://base/textures/uiresources/";
    if(url.size()>CRML_TUTORIAL_IMAGE_URL_MAX || !url.starts_with(prefix) || !url.ends_with(".png")) return false;
    const auto path=url.substr(prefix.size());
    if(path.size()<=4 || path.front()=='/' || path.find("..")!=path.npos || path.find("//")!=path.npos ||
       path.starts_with("./") || path.find("/./")!=path.npos || path.ends_with("/.png")) return false;
    for(unsigned char c:path) if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
        (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c=='/')) return false;
    return true;
}
std::string tutorial_body_markup(std::string_view text,std::string_view image_url,crml_tutorial_image_layout image) {
    std::string out;out.reserve(text.size());
    for(char c:text) switch(c) {
        case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;
        case '"':out+="&quot;";break;case '\'':out+="&#39;";break;
        case '\n':out+="<br>";break;case '\t':out+="    ";break;default:out+=c;
    }
    if(!tutorial_image_url_valid(image_url) || !layout_ok(image)) return out;
    // cohinline paragraphs flatten nested layout. Carry inert metadata there;
    // the native document binding creates a separate image row and text paragraph.
    // Without that binding this span still displays readable text, never an inline image.
    std::string markup="<span data-crml-tutorial-image=\"";markup+=image_url;
    markup+="\" data-crml-tutorial-layout=\"";
    markup+=std::to_string(image.position)+","+std::to_string(image.alignment)+","+
        std::to_string(image.width_percent?image.width_percent:100)+","+
        std::to_string(image.max_height_vh?image.max_height_vh:18)+","+
        std::to_string(image.gap_vh?image.gap_vh:1);
    return markup+"\">"+out+"</span>";

}
}
