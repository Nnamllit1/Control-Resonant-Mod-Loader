#include "mod_drawing.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <span>

namespace crml {
namespace {
bool between(float value,float low,float high) {return std::isfinite(value)&&value>=low&&value<=high;}
template<size_t N> bool text_valid(const char (&text)[N],bool empty=false) {
    const auto end=static_cast<const char*>(std::memchr(text,0,sizeof(text)));
    if(!end)return false;if(end==text)return empty;
    wchar_t decoded[N]{};
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,static_cast<int>(end-text),decoded,int(N));
    if(!count)return false;
    for(int i=0;i<count;++i)if(decoded[i]<32 || (decoded[i]>=127&&decoded[i]<=159) || decoded[i]==0x2028 || decoded[i]==0x2029)return false;
    return true;
}
bool valid(const crml_drawing_frame& f) {
    if(f.version!=CRML_DRAWING_VERSION || f.size!=sizeof(f) || f.lifetime_ms<100 || f.lifetime_ms>1000 ||
       f.segment_count>CRML_DRAWING_SEGMENTS_MAX || f.label_count>CRML_DRAWING_LABELS_MAX || (!f.segment_count&&!f.label_count) ||
       f.reserved[0] || f.reserved[1] || f.reserved[2] || !between(f.x,0,1) || !between(f.y,0,1) ||
       !between(f.width,.05f,1) || !between(f.height,.05f,1) || f.x+f.width>1 || f.y+f.height>1)return false;
    for(uint32_t i=0;i<f.segment_count;++i) {
        const auto& s=f.segments[i];
        if(!between(s.x1,0,1)||!between(s.y1,0,1)||!between(s.x2,0,1)||!between(s.y2,0,1)||!between(s.width_vh,.05f,1))return false;
    }
    for(uint32_t i=0;i<f.label_count;++i) {
        const auto& l=f.labels[i];
        if(!between(l.x,0,1)||!between(l.y,0,1)||!between(l.font_vh,1,5)||!text_valid(l.text))return false;
    }
    return true;
}
bool same_geometry(const crml_drawing_frame& a,const crml_drawing_frame& b) {
    if(a.x!=b.x||a.y!=b.y||a.width!=b.width||a.height!=b.height||
       a.segment_count!=b.segment_count||a.label_count!=b.label_count)return false;
    for(uint32_t i=0;i<a.segment_count;++i){const auto& x=a.segments[i];const auto& y=b.segments[i];
        if(x.x1!=y.x1||x.y1!=y.y1||x.x2!=y.x2||x.y2!=y.y2||x.width_vh!=y.width_vh||x.rgba!=y.rgba)return false;}
    for(uint32_t i=0;i<a.label_count;++i){const auto& x=a.labels[i];const auto& y=b.labels[i];
        if(x.x!=y.x||x.y!=y.y||x.font_vh!=y.font_vh||x.rgba!=y.rgba||std::strcmp(x.text,y.text)!=0)return false;}
    return true;
}
bool number(std::string_view text,uint64_t& out) {
    if(text.empty() || text[0]<'1' || text[0]>'9')return false;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),out);
    return result.ec==std::errc{}&&result.ptr==text.data()+text.size();
}
void quote(std::string& out,std::string_view text) {
    out+='"';for(const auto c:text){if(c=='"'||c=='\\')out+='\\';out+=c;}out+='"';
}
void scalar(std::string& out,float value) {
    char buffer[32];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(result.ec!=std::errc{})throw std::runtime_error("drawing scalar");
    out.append(buffer,result.ptr);
}
}
struct ModDrawing::Impl {
    struct Layer {crml_drawing_frame frame{};crml_map_frame source{};uint64_t revision{},deadline{},last_publish{};bool visible{},published{};};
    struct Owner {Layer map,sonar;std::string id;crml_drawing_frame frame{};uint64_t revision{},deadline{},last_publish{};bool visible{},published{};
        crml_map_annotations_v2 annotations{};uint64_t annotations_deadline{},annotations_last{};bool annotations_visible{},annotations_published{};
        std::deque<crml_map_annotation_event> events;uint64_t native_order{},placement_order{};
        std::array<uint64_t,16> hovered_deletes{};uint32_t hovered_delete_count{};uint64_t hovered_delete_revision{},hovered_delete_context{};
    };
    std::mutex mutex;Clock clock;std::map<uint64_t,Owner> owners;
    uint64_t page{},sequence{},revision{},map_context{},map_deadline{},sonar_context{},sonar_deadline{};bool enabled{},projection_valid{};
    std::array<uint32_t,24> ui_status{};uint64_t ui_status_time{},ui_reports{};
    bool editor_supported{};uint64_t editor_owner{},editor_revision{},editor_context{},editor_deadline{};
    uint64_t hover_owner{},hover_revision{},hover_context{},hover_world_id{},hover_deadline{};
    std::array<float,4> hover_box{};
    uint64_t claim_sequence{},native_owner{},placement_owner{};
    sonar_projection::Snapshot sonar_projection{};
    map_projection::District projection{};std::array<float,4> map_rect{};
    explicit Impl(Clock source):clock(source?std::move(source):Clock([]{return GetTickCount64();})){}
    void clear_hover(){hover_owner=hover_revision=hover_context=hover_world_id=hover_deadline=0;hover_box={};}
    bool hover_world_visible(const Owner& item,uint64_t id) const {
        if(!item.annotations_visible||item.annotations.context!=map_context)return false;
        const auto& f=item.annotations;
        for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];
            if(a.id!=id||(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)||!(a.flags&CRML_MAP_ANNOTATION_EDITABLE))continue;
            std::array<float,2> uv{};
            if(!map_projection::normalized(projection,{a.position[0],a.position[1],a.position[2]},uv)||
               !between(uv[0],0,1)||!between(uv[1],0,1))return false;
            float distance=0;for(unsigned j=0;j<3;++j){const float d=a.position[j]-f.origin[j];distance+=d*d;}
            return a.distance==0||distance<=a.distance*a.distance;
        }
        return false;
    }
    void arbitrate() {
        auto choose=[&](uint64_t& retained,uint32_t flag,uint64_t Owner::*order){
            auto eligible=[&](const Owner& item){return item.annotations_visible&&(item.annotations.flags&flag)&&item.*order;};
            auto current=owners.find(retained);if(current!=owners.end()&&eligible(current->second))return;
            retained=0;uint64_t first=UINT64_MAX;
            for(const auto& [owner,item]:owners)if(eligible(item)&&item.*order<first){first=item.*order;retained=owner;}
        };
        choose(native_owner,CRML_MAP_ANNOTATIONS_NATIVE_MARKERS,&Owner::native_order);
        choose(placement_owner,CRML_MAP_ANNOTATIONS_PLACE_ACTION,&Owner::placement_order);
    }
    void expire(uint64_t now){
        for(auto& [owner,item]:owners){
            if(item.visible&&now>=item.deadline)item.visible=false;
            if(item.map.visible&&(now>=item.map.deadline||now>=map_deadline))item.map.visible=false;
            if(item.sonar.visible&&(now>=item.sonar.deadline||now>=sonar_deadline))item.sonar.visible=false;
            if(item.annotations_visible&&(now>=item.annotations_deadline||(now>=map_deadline&&now>=sonar_deadline))){item.annotations_visible=false;item.events.clear();item.hovered_delete_count=0;}
            if(!item.annotations_visible)item.native_order=item.placement_order=0;
        }
        arbitrate();
        const auto hovered=owners.find(hover_owner);
        if(now>=hover_deadline||now>=map_deadline||hovered==owners.end()||!hovered->second.annotations_visible||
           hovered->second.annotations.revision!=hover_revision||hovered->second.annotations.context!=hover_context)
            clear_hover();
    }
    void clear(){for(auto& [owner,item]:owners){item.visible=false;item.map.visible=false;item.sonar.visible=false;item.annotations_visible=false;item.events.clear();item.hovered_delete_count=0;item.native_order=item.placement_order=0;}native_owner=placement_owner=0;clear_hover();}
    size_t visible() const {size_t count{};for(const auto& [owner,item]:owners)count+=size_t(item.visible)+size_t(item.map.visible)+size_t(item.sonar.visible);return count;}
};
ModDrawing::ModDrawing(Clock clock):impl_(std::make_unique<Impl>(std::move(clock))){}
ModDrawing::~ModDrawing()=default;
bool ModDrawing::attach(uint64_t owner,std::string_view id,const ModMetadata& metadata) {
    if(!owner||id.empty()||id.size()>64||id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=id.npos||!metadata.valid())return false;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(s.owners.size()>=32||s.owners.contains(owner))return false;
    for(const auto& [key,item]:s.owners)if(item.id==id)return false;
    Impl::Owner item;item.id=id;s.owners.emplace(owner,std::move(item));return true;
}
void ModDrawing::detach(uint64_t owner){std::lock_guard lock(impl_->mutex);if(impl_->hover_owner==owner)impl_->clear_hover();impl_->owners.erase(owner);impl_->expire(impl_->clock());}
void ModDrawing::cancel(uint64_t owner){hide(owner);map_hide(owner);map_hide_target(owner,CRML_MAP_SONAR);annotations_hide(owner);}
bool ModDrawing::available(){std::lock_guard lock(impl_->mutex);return impl_->enabled;}
void ModDrawing::enable_renderer(bool enabled) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!enabled){s.projection_valid=false;s.clear();s.map_deadline=s.sonar_deadline=0;s.sequence=0;if(s.page!=UINT64_MAX)++s.page;}s.enabled=enabled;
}
uint64_t ModDrawing::open_page() {
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!s.enabled||s.page==UINT64_MAX)return 0;
    s.clear();s.editor_deadline=0;s.sequence=0;s.ui_status={};s.ui_reports=s.ui_status_time=0;return ++s.page;
}
void ModDrawing::enable_map_editor_input(bool enabled) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.editor_supported=enabled;s.editor_deadline=0;
}
bool ModDrawing::map_editor_input_active() noexcept {
    try {
        auto& s=*impl_;std::lock_guard lock(s.mutex);
        const auto now=s.clock();const auto it=s.owners.find(s.editor_owner);
        return s.enabled&&s.editor_supported&&now<s.editor_deadline&&now<s.map_deadline&&it!=s.owners.end()&&
            it->second.annotations_visible&&now<it->second.annotations_deadline&&
            it->second.annotations.revision==s.editor_revision&&it->second.annotations.context==s.editor_context;
    }catch(...){return false;}
}
int ModDrawing::publish(uint64_t owner,const crml_drawing_frame& frame) {
    if(!valid(frame))return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
    const auto found=s.owners.find(owner);if(!s.enabled||found==s.owners.end())return -1;
    auto& item=found->second;
    if(!item.visible&&s.visible()>=CRML_DRAWING_VISIBLE_MAX)return -2;
    if(item.published&&(now<item.last_publish||now-item.last_publish<100))return -4;
    const bool changed=!item.visible||!same_geometry(item.frame,frame);
    if((changed&&s.revision==UINT64_MAX)||now>UINT64_MAX-frame.lifetime_ms)return -5;
    // No mutation precedes complete validation/admission. Copy the whole frame
    // while holding the lock so readers never see a partially replaced route.
    item.frame=frame;if(changed)item.revision=++s.revision;item.deadline=now+frame.lifetime_ms;
    item.last_publish=now;item.published=true;item.visible=true;return 1;
}
int ModDrawing::hide(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());
    const auto found=s.owners.find(owner);if(found==s.owners.end())return -1;
    const bool visible=found->second.visible;found->second.visible=false;return visible?1:0;
}

void ModDrawing::update_map(const map_projection::District& projection,const std::array<float,4>& rect) noexcept {
    try {
        std::array<float,2> probe{};
        if(!map_projection::normalized(projection,{0,0,0},probe))return;
        for(float v:rect)if(!std::isfinite(v)||std::abs(v)>100000)return;
        if(rect[2]<=0||rect[3]<=0)return;
        auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||!s.enabled)return;
        const auto now=s.clock();
        const auto& a=s.projection;const auto& b=projection;
        const bool changed=!s.projection_valid||a.rotation!=b.rotation||a.translation!=b.translation||
            a.minimum!=b.minimum||a.maximum!=b.maximum||a.offset!=b.offset||a.scale!=b.scale;
        if(s.map_context==UINT64_MAX||now>UINT64_MAX-500)return;
        if(changed){++s.map_context;s.clear_hover();for(auto& [owner,item]:s.owners){item.map.visible=false;item.annotations_visible=false;item.events.clear();item.hovered_delete_count=0;}}
        s.projection=projection;s.projection_valid=true;s.map_rect=rect;s.map_deadline=now+500;
    }catch(...){/* Native producer must always retain callthrough. */}
}
void ModDrawing::update_sonar(const sonar_projection::Snapshot& projection) noexcept {
    if(!sonar_projection::valid(projection))return;
    try {
        auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||!s.enabled)return;
        const auto now=s.clock();
        if(s.sonar_context==UINT64_MAX||now>UINT64_MAX-500)return;
        if(now>=s.sonar_deadline){++s.sonar_context;for(auto& [owner,item]:s.owners)item.sonar.visible=false;}
        s.sonar_projection=projection;s.sonar_deadline=now+500;
    }catch(...){/* Never interrupt the native producer. */}
}
int ModDrawing::projection_read(uint64_t owner,crml_map_projection& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!s.enabled||!s.owners.contains(owner)||s.clock()>=s.map_deadline||!s.projection_valid)return -1;
    std::array<float,2> base{},point{};if(!map_projection::normalized(s.projection,{0,0,0},base))return -1;
    crml_map_projection result{};result.version=1;result.size=sizeof(result);result.context=s.map_context;
    for(unsigned axis=0;axis<3;++axis){std::array<float,3> basis{};basis[axis]=1;
        if(!map_projection::normalized(s.projection,basis,point))return -1;
        for(unsigned row=0;row<2;++row)result.world_to_map[row*4+axis]=point[row]-base[row];
    }
    for(unsigned row=0;row<2;++row)result.world_to_map[row*4+3]=base[row];
    std::copy(s.map_rect.begin(),s.map_rect.end(),result.rect);out=result;return 1;
}
int ModDrawing::map_read(uint64_t owner,crml_map_state& out) {return map_read_target(owner,CRML_MAP_FULL,out);}
int ModDrawing::map_read_target(uint64_t owner,uint32_t target,crml_map_state& out) {
    out={};if(target!=CRML_MAP_FULL&&target!=CRML_MAP_SONAR)return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    const auto deadline=target==CRML_MAP_FULL?s.map_deadline:s.sonar_deadline;
    const auto context=target==CRML_MAP_FULL?s.map_context:s.sonar_context;
    if(!s.enabled||!s.owners.contains(owner)||s.clock()>=deadline||!context)return -1;
    out={CRML_MAP_VERSION,sizeof(out),target,0,context};return 1;
}
namespace {
bool world_valid(const float* p){for(size_t i=0;i<3;++i)if(!between(p[i],-100000000,100000000))return false;return true;}
// Liang-Barsky: intersect the segment with the district rectangle. Clamping
// endpoints independently would invent trails along the border.
bool clip(std::array<float,2>& a,std::array<float,2>& b) {
    double first=0,last=1,dx=double(b[0])-a[0],dy=double(b[1])-a[1];
    const double p[]{-dx,dx,-dy,dy},q[]{a[0],1-double(a[0]),a[1],1-double(a[1])};
    for(size_t i=0;i<4;++i){
        if(p[i]==0){if(q[i]<0)return false;continue;}
        const double r=q[i]/p[i];
        if(p[i]<0)first=std::max(first,r);else last=std::min(last,r);
        if(first>last)return false;
    }
    const auto origin=a;
    for(size_t i=0;i<2;++i){const double delta=i?dy:dx;a[i]=float(std::clamp(double(origin[i])+first*delta,0.,1.));b[i]=float(std::clamp(double(origin[i])+last*delta,0.,1.));}
    return a!=b;
}
bool project_frame(const crml_map_frame& f,const map_projection::District& district,const std::array<float,4>& rect,
                   const sonar_projection::Snapshot& sonar_snapshot,crml_drawing_frame& projected) {
    const bool sonar=f.target==CRML_MAP_SONAR;
    projected={};projected.version=1;projected.size=sizeof(projected);projected.lifetime_ms=f.lifetime_ms;
    projected.x=sonar?0:rect[0];projected.y=sonar?0:rect[1];projected.width=sonar?1:rect[2];projected.height=sonar?1:rect[3];
    const auto project=[&](const std::array<float,3>& world,std::array<float,2>& uv){
        float height{};return sonar?sonar_projection::project(sonar_snapshot,world,uv,height):map_projection::normalized(district,world,uv);
    };
    for(uint32_t i=0;i<f.segment_count;++i){
        const auto& l=f.segments[i];std::array<float,2> a{},b{};
        std::array<float,3> from{l.from[0],l.from[1],l.from[2]},to{l.to[0],l.to[1],l.to[2]};
        if(sonar&&!sonar_projection::clip_segment(sonar_snapshot,from,to))continue;
        if(!project(from,a)||!project(to,b))return false;
        if(clip(a,b))projected.segments[projected.segment_count++]={a[0],a[1],b[0],b[1],l.width_vh,l.rgba};
    }
    for(uint32_t i=0;i<f.label_count;++i){
        const auto& l=f.labels[i];std::array<float,2> a{};
        std::array<float,3> point{l.position[0],l.position[1],l.position[2]},same=point;
        if(sonar&&!sonar_projection::clip_segment(sonar_snapshot,point,same))continue;
        if(!project(point,a))return false;
        if(a[0]<0||a[0]>1||a[1]<0||a[1]>1)continue;
        auto& out=projected.labels[projected.label_count++];out={a[0],a[1],l.font_vh,l.rgba,{}};std::memcpy(out.text,l.text,sizeof(out.text));
    }
    return true;
}
}
int ModDrawing::map_publish(uint64_t owner,const crml_map_frame& f) {
    if(f.version!=CRML_MAP_VERSION||f.size!=sizeof(f)||(f.target!=CRML_MAP_FULL&&f.target!=CRML_MAP_SONAR)||!f.context||f.lifetime_ms<100||f.lifetime_ms>1000||
       f.segment_count>128||f.label_count>32||(!f.segment_count&&!f.label_count))return -3;
    for(uint32_t i=0;i<f.segment_count;++i){const auto& l=f.segments[i];if(!world_valid(l.from)||!world_valid(l.to)||!between(l.width_vh,.05f,1))return -3;}
    for(uint32_t i=0;i<f.label_count;++i){const auto& l=f.labels[i];if(!world_valid(l.position)||!between(l.font_vh,1,5)||!text_valid(l.text))return -3;}
    auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
    const bool sonar=f.target==CRML_MAP_SONAR;
    const auto found=s.owners.find(owner);if(!s.enabled||found==s.owners.end()||now>=(sonar?s.sonar_deadline:s.map_deadline))return -1;
    if(f.context!=(sonar?s.sonar_context:s.map_context))return -6;
    auto& item=sonar?found->second.sonar:found->second.map;
    if(!item.visible&&s.visible()>=CRML_DRAWING_VISIBLE_MAX)return -2;
    if(item.published&&(now<item.last_publish||now-item.last_publish<100))return -4;
    crml_drawing_frame projected{};
    if(!project_frame(f,s.projection,s.map_rect,s.sonar_projection,projected))return -3;
    const bool has_pixels=projected.segment_count||projected.label_count;
    const bool changed=!item.visible||!same_geometry(item.frame,projected);
    if((changed&&s.revision==UINT64_MAX)||now>UINT64_MAX-f.lifetime_ms)return -5;
    item.frame=projected;item.source=f;if(changed)item.revision=++s.revision;item.deadline=now+f.lifetime_ms;
    // An off-screen sonar frame still owns its bounded slot until expiry/hide.
    // Camera movement may bring its world geometry back without guest republish.
    item.last_publish=now;item.published=true;item.visible=sonar||has_pixels;return has_pixels?1:0;
}
int ModDrawing::map_hide(uint64_t owner) {return map_hide_target(owner,CRML_MAP_FULL);}
int ModDrawing::map_hide_target(uint64_t owner,uint32_t target) {
    if(target!=CRML_MAP_FULL&&target!=CRML_MAP_SONAR)return -3;
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());
    const auto found=s.owners.find(owner);if(found==s.owners.end())return -1;
    auto& layer=target==CRML_MAP_FULL?found->second.map:found->second.sonar;
    const bool visible=layer.visible;layer.visible=false;return visible?1:0;
}
int ModDrawing::poll(uint64_t page,uint64_t sequence,std::vector<Surface>& output) {
    output.clear();if(!page||!sequence)return 400;
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(!s.enabled)return 503;if(page!=s.page)return 403;if(sequence<=s.sequence)return 409;
    s.clear_hover(); // Each authenticated poll replaces the pointer selection.
    const auto now=s.clock();s.expire(now);
    for(auto& [owner,item]:s.owners){
        if(item.sonar.visible){
            crml_drawing_frame projected{};
            if(item.sonar.source.context!=s.sonar_context||
               !project_frame(item.sonar.source,s.projection,s.map_rect,s.sonar_projection,projected))item.sonar.visible=false;
            else if(!same_geometry(item.sonar.frame,projected)){
                if(s.revision==UINT64_MAX)item.sonar.visible=false;
                else {item.sonar.frame=projected;item.sonar.revision=++s.revision;}
            }
        }
        if(item.visible)output.push_back({owner,item.revision,static_cast<uint32_t>(item.deadline-now),item.frame});
        if(item.map.visible)output.push_back({owner,item.map.revision,static_cast<uint32_t>(std::min(item.map.deadline,s.map_deadline)-now),item.map.frame,CRML_MAP_FULL});
        if(item.sonar.visible&&(item.sonar.frame.segment_count||item.sonar.frame.label_count))output.push_back({owner,item.sonar.revision,static_cast<uint32_t>(std::min(item.sonar.deadline,s.sonar_deadline)-now),item.sonar.frame,CRML_MAP_SONAR});
    }
    s.sequence=sequence;return 200;
}
namespace {
bool annotation_valid(const crml_map_annotation& a) {
    const char c=a.symbol[0];
    return world_valid(a.position)&&a.flags<=3 && (!(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)||(a.id>=1&&a.id<=6&&a.distance==0&&a.position[2]==0))&&between(a.distance,0,1000000)&&
        a.symbol[1]==0&&((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='*'||c=='+'||c=='!'||c=='?')&&
        text_valid(a.name)&&text_valid(a.description,true);
}
template<class T> bool parse(std::string_view text,T& out) {
    if(text.empty())return false;const auto r=std::from_chars(text.data(),text.data()+text.size(),out);
    return r.ec==std::errc{}&&r.ptr==text.data()+text.size();
}
template<size_t N> bool unhex(std::string_view text,char (&out)[N]) {
    if(text.empty()||text[0]!='t'||text.size()%2!=1||(text.size()-1)/2>=N)return false;
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    for(size_t i=1;i<text.size();i+=2){const int a=digit(text[i]),b=digit(text[i+1]);if(a<0||b<0||!(a||b))return false;out[(i-1)/2]=char(a*16+b);}
    return true;
}
}
int ModDrawing::annotations_publish(uint64_t owner,const crml_map_annotations& f) {
    if(f.version!=1||f.size!=sizeof(f)||f.count>CRML_MAP_ANNOTATIONS_MAX||f.flags>3)return -3;
    crml_map_annotations_v2 copy{};std::memcpy(&copy,&f,sizeof(f));copy.size=sizeof(copy);
    return annotations_publish_copied(owner,copy);
}
int ModDrawing::annotations_publish_v2(uint64_t owner,const crml_map_annotations_v2& f) {
    if(f.version!=2)return -3;return annotations_publish_copied(owner,f);
}
int ModDrawing::annotations_publish_copied(uint64_t owner,const crml_map_annotations_v2& f) {
    if((f.version<1||f.version>3)||f.size!=sizeof(f)||f.count>(f.version==1?CRML_MAP_ANNOTATIONS_MAX:CRML_MAP_ANNOTATIONS_V2_MAX)||f.flags>(f.version==1?3u:7u)||!f.context||!f.revision||!world_valid(f.origin))return -3;
    double norm{};for(float v:f.up){if(!std::isfinite(v))return -3;norm+=double(v)*v;}
    if(norm<.98||norm>1.02)return -3;
    for(unsigned i=0;i<f.count;++i){if(!f.items[i].id||!annotation_valid(f.items[i]))return -3;for(unsigned j=0;j<i;++j)if(f.items[i].id==f.items[j].id&&(f.version<3||((f.items[i].flags^f.items[j].flags)&CRML_MAP_ANNOTATION_NATIVE_MARKER)==0))return -3;}
    auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
    const auto it=s.owners.find(owner);if(!s.enabled||it==s.owners.end()||(now>=s.map_deadline&&now>=s.sonar_deadline))return -1;
    if(f.context!=s.map_context)return -6;auto& item=it->second;
    unsigned active=0;for(const auto& [key,value]:s.owners)active+=value.annotations_visible?1:0;
    if(!item.annotations_visible&&active>=4)return -2;
    if(item.annotations_published&&(now<item.annotations_last||now-item.annotations_last<100))return -4;
    if(now>UINT64_MAX-600)return -5;
    if(item.annotations_visible){
        const auto& old=item.annotations;
        if(f.revision<old.revision)return -6;
        if(f.revision==old.revision&&(f.version!=old.version||f.flags!=old.flags||f.count!=old.count||std::memcmp(f.items,old.items,f.count*sizeof(f.items[0]))!=0))return -6;
    }
    const bool new_native=(f.flags&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS)&&!item.native_order;
    const bool new_placement=(f.flags&CRML_MAP_ANNOTATIONS_PLACE_ACTION)&&!item.placement_order;
    if(s.claim_sequence>UINT64_MAX-uint64_t(new_native)-uint64_t(new_placement))return -5;
    if(!(f.flags&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS))item.native_order=0;
    else if(new_native)item.native_order=++s.claim_sequence;
    if(!(f.flags&CRML_MAP_ANNOTATIONS_PLACE_ACTION))item.placement_order=0;
    else if(new_placement)item.placement_order=++s.claim_sequence;
    if(item.annotations.version!=f.version)item.events.clear();
    if(item.hovered_delete_revision!=f.revision||item.hovered_delete_context!=f.context)item.hovered_delete_count=0;
    item.annotations=f;item.annotations_deadline=now+600;item.annotations_last=now;item.annotations_published=true;item.annotations_visible=true;s.arbitrate();return 1;
}
int ModDrawing::annotations_publish_v3(uint64_t owner,const crml_map_annotations_v3& f) {
    if(f.version!=3||f.size!=sizeof(f)||f.reserved||f.count>128||f.attachment_count>6||f.count+f.attachment_count>128)return -3;
    crml_map_annotations_v2 copy{};copy.version=3;copy.size=sizeof(copy);copy.count=f.count+f.attachment_count;
    copy.flags=f.flags;copy.context=f.context;copy.revision=f.revision;
    std::memcpy(copy.origin,f.origin,sizeof(copy.origin));std::memcpy(copy.up,f.up,sizeof(copy.up));
    for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];if(a.flags>CRML_MAP_ANNOTATION_EDITABLE)return -3;
        static_assert(sizeof(a)==sizeof(copy.items[i]));std::memcpy(&copy.items[i],&a,sizeof(a));
    }
    for(unsigned i=0;i<f.attachment_count;++i){const auto& a=f.attachments[i];
        if(a.native_slot<1||a.native_slot>6||a.flags>CRML_MAP_ANNOTATION_EDITABLE||a.reserved)return -3;
        auto& out=copy.items[f.count+i];out.id=a.native_slot;out.flags=a.flags|CRML_MAP_ANNOTATION_NATIVE_MARKER;out.rgba=a.rgba;
        out.position[0]=a.map_pixels[0];out.position[1]=a.map_pixels[1];
        std::memcpy(out.symbol,a.symbol,sizeof(out.symbol));std::memcpy(out.name,a.name,sizeof(out.name));std::memcpy(out.description,a.description,sizeof(out.description));
    }
    return annotations_publish_copied(owner,copy);
}
int ModDrawing::annotations_status(uint64_t owner,crml_map_annotation_status& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());const auto it=s.owners.find(owner);
    if(!s.enabled||it==s.owners.end())return -1;const auto& item=it->second;if(!item.annotations_visible)return 0;
    out.version=1;out.size=sizeof(out);out.context=item.annotations.context;out.revision=item.annotations.revision;
    out.requested=item.annotations.flags&(CRML_MAP_ANNOTATIONS_NATIVE_MARKERS|CRML_MAP_ANNOTATIONS_PLACE_ACTION);
    out.granted=(s.native_owner==owner?CRML_MAP_ANNOTATIONS_NATIVE_MARKERS:0u)|(s.placement_owner==owner?CRML_MAP_ANNOTATIONS_PLACE_ACTION:0u);return 1;
}
int ModDrawing::annotations_next_v2(uint64_t owner,crml_map_annotation_event_v2& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());const auto it=s.owners.find(owner);
    if(!s.enabled||it==s.owners.end())return -1;auto& item=it->second;
    if(item.annotations.version&&item.annotations.version!=3)return -3;if(item.events.empty())return 0;
    const auto& event=item.events.front();const auto& a=event.item;
    out.version=2;out.action=event.action;out.revision=event.revision;out.context=item.annotations.context;
    if(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER){
        out.target=2;auto& attachment=out.attachment;attachment.native_slot=static_cast<uint32_t>(a.id);
        attachment.flags=a.flags&CRML_MAP_ANNOTATION_EDITABLE;attachment.rgba=a.rgba;
        attachment.map_pixels[0]=a.position[0];attachment.map_pixels[1]=a.position[1];
        std::memcpy(attachment.symbol,a.symbol,sizeof(a.symbol));std::memcpy(attachment.name,a.name,sizeof(a.name));std::memcpy(attachment.description,a.description,sizeof(a.description));
    }else{out.target=1;std::memcpy(&out.world,&a,sizeof(a));}
    item.events.pop_front();return 1;
}
int ModDrawing::annotations_hide(uint64_t owner) {
    auto& s=*impl_;std::lock_guard lock(s.mutex);auto it=s.owners.find(owner);if(it==s.owners.end())return -1;
    auto& item=it->second;const bool was=item.annotations_visible;item.annotations_visible=false;item.events.clear();item.hovered_delete_count=0;if(s.hover_owner==owner)s.clear_hover();s.expire(s.clock());return was?1:0;
}
bool ModDrawing::placement_action(const std::array<float,2>& point,bool overflow) noexcept {
    try {
        auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
        if(!s.enabled||!s.projection_valid||now>=s.map_deadline||now<s.editor_deadline||
           !std::isfinite(point[0])||!std::isfinite(point[1]))return false;
        const std::array<float,2> uv{(point[0]-s.map_rect[0])/s.map_rect[2],(point[1]-s.map_rect[1])/s.map_rect[3]};
        if(!between(uv[0],0,1)||!between(uv[1],0,1))return false;
        for(auto& [owner,item]:s.owners){const auto& f=item.annotations;
            if(owner!=s.placement_owner||!item.annotations_visible||f.context!=s.map_context||!(f.flags&CRML_MAP_ANNOTATIONS_PLACE_ACTION))continue;
            // Only one opted-in owner handles the action. Do not replay an
            // action while its prior revision is still waiting for the guest.
            if(!item.events.empty()){
                for(const auto& event:item.events){std::array<float,2> at{};
                    if((event.item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)||
                       (event.action!=CRML_MAP_ANNOTATION_CREATE&&event.action!=CRML_MAP_ANNOTATION_DELETE)||
                       !map_projection::normalized(s.projection,{event.item.position[0],event.item.position[1],event.item.position[2]},at))continue;
                    const float dx=(at[0]-uv[0])*s.map_rect[2],dy=(at[1]-uv[1])*s.map_rect[3];
                    if(dx*dx+dy*dy<=64)return true; // Coalesce the same action until consumed.
                }
                return false;
            }
            for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];std::array<float,2> at{};
                if((a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)||!(a.flags&CRML_MAP_ANNOTATION_EDITABLE)||
                   !map_projection::normalized(s.projection,{a.position[0],a.position[1],a.position[2]},at))continue;
                float distance=0;for(unsigned j=0;j<3;++j){const float v=a.position[j]-f.origin[j];distance+=v*v;}
                if(a.distance>0&&distance>a.distance*a.distance)continue;
                const float dx=(at[0]-uv[0])*s.map_rect[2],dy=(at[1]-uv[1])*s.map_rect[3];
                if(dx*dx+dy*dy<=64){item.events.push_back({1,CRML_MAP_ANNOTATION_DELETE,f.revision,a});return true;}
            }
            if(!overflow||!(f.flags&CRML_MAP_ANNOTATIONS_CREATE)||f.count>=CRML_MAP_ANNOTATIONS_V2_MAX)return false;
            std::array<float,3> world{};
            if(!map_projection::on_plane(s.projection,uv,{f.origin[0],f.origin[1],f.origin[2]},{f.up[0],f.up[1],f.up[2]},world))return false;
            crml_map_annotation a{};a.flags=CRML_MAP_ANNOTATION_EDITABLE;a.rgba=0xffe09aff;
            std::copy(world.begin(),world.end(),a.position);std::strcpy(a.symbol,"*");std::strcpy(a.name,"Marker");
            item.events.push_back({1,CRML_MAP_ANNOTATION_CREATE,f.revision,a});return true;
        }
    }catch(...){return false;}
    return false;
}
bool ModDrawing::hovered_annotation_action(const std::array<float,2>& cursor) noexcept {
    try {
        auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
        if(!s.enabled||!s.hover_owner||s.hover_owner!=s.placement_owner||now>=s.map_deadline||!s.projection_valid||
           !between(cursor[0],s.hover_box[0],s.hover_box[2])||!between(cursor[1],s.hover_box[1],s.hover_box[3])||
           (s.editor_supported&&now<s.editor_deadline))return false;
        const auto found=s.owners.find(s.hover_owner);if(found==s.owners.end())return false;
        auto& item=found->second;const auto& f=item.annotations;
        if(!item.annotations_visible||!(f.flags&CRML_MAP_ANNOTATIONS_PLACE_ACTION)||now>=item.annotations_deadline||f.context!=s.map_context||
           f.context!=s.hover_context||f.revision!=s.hover_revision||
           !s.hover_world_visible(item,s.hover_world_id))return false;
        if(item.hovered_delete_revision!=f.revision||item.hovered_delete_context!=f.context){
            item.hovered_delete_revision=f.revision;item.hovered_delete_context=f.context;item.hovered_delete_count=0;
        }
        for(unsigned i=0;i<item.hovered_delete_count;++i)if(item.hovered_deletes[i]==s.hover_world_id)return true;
        if(item.hovered_delete_count>=item.hovered_deletes.size())return true; // Keep X off stock slots while this owner catches up.
        for(const auto& pending:item.events)if(pending.action==CRML_MAP_ANNOTATION_DELETE&&
            pending.revision==f.revision&&pending.item.id==s.hover_world_id&&
            !(pending.item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)){
            item.hovered_deletes[item.hovered_delete_count++]=s.hover_world_id;return true;
        }
        if(item.events.size()>=16)return true; // Backpressure must not redirect a destructive action to stock.
        for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];
            if(a.id!=s.hover_world_id||(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER))continue;
            item.events.push_back({1,CRML_MAP_ANNOTATION_DELETE,f.revision,a});
            item.hovered_deletes[item.hovered_delete_count++]=s.hover_world_id;return true;
        }
    }catch(...){/* Native producer retains stock callthrough on failure. */}
    return false;
}
int ModDrawing::annotations_next(uint64_t owner,crml_map_annotation_event& out) {
    out={};auto& s=*impl_;std::lock_guard lock(s.mutex);s.expire(s.clock());auto it=s.owners.find(owner);
    if(!s.enabled||it==s.owners.end())return -1;if(it->second.annotations.version==3)return -3;auto& queue=it->second.events;if(queue.empty())return 0;
    out=queue.front();queue.pop_front();return 1;
}
int ModDrawing::annotation_exchange(std::string_view url,bool typed) {
    std::array<std::string_view,15> storage{};std::span<std::string_view> parts(storage.data(),typed?15:14);
    for(size_t i=0;i<parts.size();++i){const auto slash=url.find('/');if(i+1<parts.size()&&slash==url.npos)return 400;if(i+1==parts.size()&&slash!=url.npos)return 400;parts[i]=url.substr(0,slash);if(slash!=url.npos)url.remove_prefix(slash+1);}
    uint64_t page{},seq{},owner{},revision{},context{};uint32_t action{};crml_map_annotation proposed{};float x{},y{};
    if(!number(parts[0],page)||!number(parts[1],seq)||!number(parts[2],owner)||!number(parts[3],revision)||!number(parts[4],context)||
       !parse(parts[5],action)||!parse(parts[6],proposed.id)||!parse(parts[7],proposed.rgba)||!parse(parts[8],proposed.distance)||!parse(parts[9],x)||!parse(parts[10],y)||
       !unhex(parts[11],proposed.symbol)||!unhex(parts[12],proposed.name)||!unhex(parts[13],proposed.description)||!annotation_valid(proposed)||!between(x,0,1)||!between(y,0,1)||action<1||action>4)return 400;
    uint32_t target=action==CRML_MAP_ANNOTATION_ATTACH?2u:1u;
    if(typed&&(!parse(parts[14],target)||(target!=1&&target!=2)))return 400;
    auto& s=*impl_;std::lock_guard lock(s.mutex);s.clear_hover();s.expire(s.clock());if(!s.enabled)return 503;if(page!=s.page)return 403;if(seq<=s.sequence)return 409;s.sequence=seq;
    auto it=s.owners.find(owner);if(it==s.owners.end())return 404;auto& item=it->second;const auto& f=item.annotations;
    if(s.clock()>=s.map_deadline||!item.annotations_visible||f.revision!=revision||f.context!=context)return 409;
    if((f.version==3)!=typed)return 409;
    if(item.events.size()>=16)return 429;
    if(action==CRML_MAP_ANNOTATION_ATTACH){
        if(owner!=s.native_owner||target!=2||!(f.flags&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS)||proposed.id<1||proposed.id>6||proposed.distance!=0)return 409;
        bool existing=false;for(unsigned i=0;i<f.count;++i)if(f.items[i].id==proposed.id&&(!typed||(f.items[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER))){if(!(f.items[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)||!(f.items[i].flags&CRML_MAP_ANNOTATION_EDITABLE))return 409;existing=true;}
        if(!existing&&f.count>=(f.version==1?CRML_MAP_ANNOTATIONS_MAX:CRML_MAP_ANNOTATIONS_V2_MAX))return 409;
        proposed.position[0]=s.map_rect[0]+x*s.map_rect[2];proposed.position[1]=s.map_rect[1]+y*s.map_rect[3];proposed.position[2]=0;
        proposed.flags=CRML_MAP_ANNOTATION_EDITABLE|CRML_MAP_ANNOTATION_NATIVE_MARKER;
    }else if(action==CRML_MAP_ANNOTATION_CREATE){
        if(target!=1||proposed.id||!(f.flags&CRML_MAP_ANNOTATIONS_CREATE)||f.count>=(f.version==1?CRML_MAP_ANNOTATIONS_MAX:CRML_MAP_ANNOTATIONS_V2_MAX))return 409;
        std::array<float,3> world{};
        if(!map_projection::on_plane(s.projection,{x,y},{f.origin[0],f.origin[1],f.origin[2]},{f.up[0],f.up[1],f.up[2]},world))return 409;
        for(unsigned i=0;i<3;++i)proposed.position[i]=world[i];proposed.flags=CRML_MAP_ANNOTATION_EDITABLE;
    }else{
        const crml_map_annotation* found=nullptr;for(unsigned i=0;i<f.count;++i)if(f.items[i].id==proposed.id&&(!typed||bool(f.items[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)==(target==2)))found=&f.items[i];
        if(!found)return 404;if(!(found->flags&CRML_MAP_ANNOTATION_EDITABLE))return 409;
        if((found->flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)&&(proposed.distance!=0||owner!=s.native_owner))return 409;
        std::memcpy(proposed.position,found->position,sizeof(proposed.position));proposed.flags=found->flags;
    }
    item.events.push_back({1,action,revision,proposed});return 202;
}
int ModDrawing::exchange(std::string_view url,std::string& output) noexcept {
    output.clear();if(!url.starts_with(drawing_prefix))return 0;
    try {
        if(url.size()>2048)return 400;url.remove_prefix(drawing_prefix.size());
        if(url.starts_with("edit2/")){const auto result=annotation_exchange(url.substr(6),true);output="{}";return result;}
        if(url.starts_with("edit/")){const auto result=annotation_exchange(url.substr(5));output="{}";return result;}
        std::array<uint64_t,3> editor{};std::array<uint64_t,4> hover{};std::array<float,4> hover_box{};
        std::array<uint32_t,24> status{};const auto query=url.find('?');const bool reported=query!=url.npos;
        bool hover_reported=false,box_reported=false,editor_reported=false;
        const auto bad_query=[&](){auto& s=*impl_;std::lock_guard lock(s.mutex);s.clear_hover();return 400;};
        const auto identities=[](std::string_view fields,auto& output){
            for(size_t i=0;i<output.size();++i){const auto comma=fields.find(',');
                if((i+1<output.size())!=(comma!=fields.npos)||!number(fields.substr(0,comma),output[i]))return false;
                if(comma!=fields.npos)fields.remove_prefix(comma+1);
            }
            return true;
        };
        const auto bounds=[](std::string_view fields,std::array<float,4>& output){
            for(size_t i=0;i<output.size();++i){const auto comma=fields.find(',');
                if((i+1<output.size())!=(comma!=fields.npos)||!parse(fields.substr(0,comma),output[i]))return false;
                if(comma!=fields.npos)fields.remove_prefix(comma+1);
            }
            return between(output[0],0,1)&&between(output[1],0,1)&&
                between(output[2],0,1)&&between(output[3],0,1)&&
                output[0]<output[2]&&output[1]<output[3];
        };
        if(reported){
            auto values=url.substr(query+1);url=url.substr(0,query);
            const auto suffix=values.find('&');auto counters=values.substr(0,suffix);
            values=suffix==values.npos?std::string_view{}:values.substr(suffix);
            if(!counters.starts_with("status="))return bad_query();counters.remove_prefix(7);
            for(size_t i=0;i<status.size();++i){const auto comma=counters.find(',');
                if((i+1<status.size())!=(comma!=counters.npos)||!parse(counters.substr(0,comma),status[i])||status[i]>1000000)return bad_query();
                if(comma!=counters.npos)counters.remove_prefix(comma+1);
            }
            if(values.starts_with("&hover=")){
                values.remove_prefix(7);const auto end=values.find('&');
                if(!identities(values.substr(0,end),hover))return bad_query();
                hover_reported=true;values=end==values.npos?std::string_view{}:values.substr(end);
            }
            if(values.starts_with("&hover_box=")){
                values.remove_prefix(11);const auto end=values.find('&');
                if(!bounds(values.substr(0,end),hover_box))return bad_query();
                box_reported=true;values=end==values.npos?std::string_view{}:values.substr(end);
            }
            if(hover_reported!=box_reported)return bad_query();
            if(values.starts_with("&editor=")){
                values.remove_prefix(8);
                if(!identities(values,editor))return bad_query();
                editor_reported=true;values={};
            }
            if(!values.empty())return bad_query();
        }
        const auto slash=url.find('/');if(slash==url.npos)return 400;
        uint64_t page{},sequence{};if(!number(url.substr(0,slash),page)||!number(url.substr(slash+1),sequence))return 400;
        std::vector<Surface> surfaces;const auto result=poll(page,sequence,surfaces);if(result!=200)return result;
        output="{\"surfaces\":[";bool first=true;
        for(const auto& surface:surfaces) {
            if(!first)output+=',';first=false;const auto& f=surface.frame;
            output+="{\"owner\":\""+std::to_string(surface.owner)+"\",\"revision\":\""+std::to_string(surface.revision)+"\",\"remaining_ms\":"+std::to_string(surface.remaining_ms)+",\"rect\":[";
            scalar(output,f.x);output+=',';scalar(output,f.y);output+=',';scalar(output,f.width);output+=',';scalar(output,f.height);output+="],\"segments\":[";
            for(uint32_t i=0;i<f.segment_count;++i){if(i)output+=',';const auto& l=f.segments[i];output+='[';
                scalar(output,l.x1);output+=',';scalar(output,l.y1);output+=',';scalar(output,l.x2);output+=',';scalar(output,l.y2);output+=',';scalar(output,l.width_vh);output+=',';output+=std::to_string(l.rgba);output+=']';}
            output+="],\"labels\":[";
            for(uint32_t i=0;i<f.label_count;++i){if(i)output+=',';const auto& l=f.labels[i];output+='[';
                scalar(output,l.x);output+=',';scalar(output,l.y);output+=',';scalar(output,l.font_vh);output+=',';output+=std::to_string(l.rgba);output+=',';quote(output,l.text);output+=']';}
            output+="],\"target\":"+std::to_string(surface.target)+"}";
        }
        output+="],\"annotations\":[";bool first_annotations=true;
        auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);if(page!=s.page)return 403;
        if(reported){s.ui_status=status;s.ui_status_time=now;++s.ui_reports;}
        // Every authenticated poll releases or renews focus. Page/owner/revision
        // changes and stalled UI revoke it without relying on a blur callback.
        s.editor_deadline=0;
        const auto editing=s.owners.find(editor[0]);
        if(s.editor_supported&&now<=UINT64_MAX-350&&now<s.map_deadline&&editing!=s.owners.end()&&
           editing->second.annotations_visible&&editing->second.annotations.revision==editor[1]&&editing->second.annotations.context==editor[2]){
            s.editor_owner=editor[0];s.editor_revision=editor[1];s.editor_context=editor[2];s.editor_deadline=now+350;
        }
        if(hover_reported&&!editor_reported&&!s.editor_deadline&&now<=UINT64_MAX-200&&now<s.map_deadline){
            const auto found=s.owners.find(hover[0]);
            if(found!=s.owners.end()&&hover[0]==s.placement_owner&&found->second.annotations_visible&&
               (found->second.annotations.flags&CRML_MAP_ANNOTATIONS_PLACE_ACTION)&&
               now<found->second.annotations_deadline&&found->second.annotations.revision==hover[1]&&
               found->second.annotations.context==hover[2]&&hover[2]==s.map_context&&
               s.hover_world_visible(found->second,hover[3])){
                s.hover_owner=hover[0];s.hover_revision=hover[1];s.hover_context=hover[2];
                s.hover_world_id=hover[3];s.hover_box=hover_box;s.hover_deadline=now+200;
            }
        }
        for(const auto& [owner,item]:s.owners)if(item.annotations_visible&&now<s.map_deadline){
            const auto& f=item.annotations;if(!first_annotations)output+=',';first_annotations=false;
            output+="{\"owner\":\""+std::to_string(owner)+"\",\"revision\":\""+std::to_string(f.revision)+"\",\"context\":\""+std::to_string(f.context)+"\",\"remaining_ms\":"+std::to_string(std::min(item.annotations_deadline,s.map_deadline)-now)+",\"rect\":[";
            for(unsigned i=0;i<4;++i){if(i)output+=',';scalar(output,s.map_rect[i]);}
            std::array<float,2> origin{};std::array<float,3> test{};
            const bool can_create=(f.flags&1)&&f.count<(f.version==1?CRML_MAP_ANNOTATIONS_MAX:CRML_MAP_ANNOTATIONS_V2_MAX)&&map_projection::normalized(s.projection,{f.origin[0],f.origin[1],f.origin[2]},origin)&&map_projection::on_plane(s.projection,origin,{f.origin[0],f.origin[1],f.origin[2]},{f.up[0],f.up[1],f.up[2]},test);
            output+="],\"version\":"+std::to_string(f.version)+",\"native_markers\":"+std::string(owner==s.native_owner?"true":"false")+",\"create\":"+std::string(can_create?"true":"false")+",\"items\":[";bool first_item=true;
            for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];std::array<float,2> uv{};
                const bool native=(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)!=0;
                if(native){uv={(a.position[0]-s.map_rect[0])/s.map_rect[2],(a.position[1]-s.map_rect[1])/s.map_rect[3]};}
                else if(!map_projection::normalized(s.projection,{a.position[0],a.position[1],a.position[2]},uv))continue;
                if(uv[0]<0||uv[0]>1||uv[1]<0||uv[1]>1)continue;
                float distance=0;for(unsigned j=0;j<3;++j){const float v=a.position[j]-f.origin[j];distance+=v*v;}
                if(a.distance>0&&distance>a.distance*a.distance)continue;
                if(!first_item)output+=',';first_item=false;
                output+="{\"id\":\""+std::to_string(a.id)+"\",\"x\":";scalar(output,uv[0]);output+=",\"y\":";scalar(output,uv[1]);
                output+=",\"rgba\":"+std::to_string(a.rgba)+",\"distance\":";scalar(output,a.distance);
                output+=",\"native\":"+std::string(native?"true":"false")+",\"editable\":"+std::string((a.flags&1)?"true":"false")+",\"symbol\":";quote(output,a.symbol);output+=",\"name\":";quote(output,a.name);output+=",\"description\":";quote(output,a.description);output+='}';
            }output+="]}";
        }
        output+="],\"native_metadata\":[";bool first_native=true;
        for(const auto& [owner,item]:s.owners)if(item.annotations_visible&&item.annotations.context==s.map_context&&
            (owner==s.native_owner)&&(item.annotations.flags&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS)){
            if(!first_native)output+=',';first_native=false;
            output+="{\"owner\":\""+std::to_string(owner)+"\",\"remaining_ms\":"+std::to_string(std::min(item.annotations_deadline,std::max(s.map_deadline,s.sonar_deadline))-now)+",\"items\":[";
            bool first_entry=true;
            for(unsigned i=0;i<item.annotations.count;++i){const auto& a=item.annotations.items[i];if(!(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER))continue;
                if(!first_entry)output+=',';first_entry=false;
                output+="{\"id\":\""+std::to_string(a.id)+"\",\"px\":";scalar(output,a.position[0]);output+=",\"py\":";scalar(output,a.position[1]);
                output+=",\"rgba\":"+std::to_string(a.rgba)+",\"symbol\":";quote(output,a.symbol);output+='}';
            }output+="]}";
        }
        output+="],\"sonar_annotations\":[";bool first_sonar=true;
        if(now<s.sonar_deadline)for(const auto& [owner,item]:s.owners)if(item.annotations_visible&&item.annotations.version>=2&&item.annotations.context==s.map_context){
            if(!first_sonar)output+=',';first_sonar=false;
            output+="{\"owner\":\""+std::to_string(owner)+"\",\"remaining_ms\":"+std::to_string(std::min(item.annotations_deadline,s.sonar_deadline)-now)+",\"items\":[";bool first=true;
            const auto& f=item.annotations;
            for(unsigned i=0;i<f.count;++i){const auto& a=f.items[i];if(a.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)continue;
                std::array<float,2> uv{};float height{};
                if(!sonar_projection::project(s.sonar_projection,{a.position[0],a.position[1],a.position[2]},uv,height))continue;
                if(uv[0]<0||uv[0]>1||uv[1]<0||uv[1]>1)continue;
                if((uv[0]-.5f)*(uv[0]-.5f)+(uv[1]-.5f)*(uv[1]-.5f)>.25f)continue;
                float distance=0;for(unsigned j=0;j<3;++j){const float d=a.position[j]-s.sonar_projection.current.origin[j];distance+=d*d;}
                if(a.distance>0&&distance>a.distance*a.distance)continue;
                if(!first)output+=',';first=false;
                output+="{\"id\":\""+std::to_string(a.id)+"\",\"x\":";scalar(output,uv[0]);output+=",\"y\":";scalar(output,uv[1]);
                output+=",\"rgba\":"+std::to_string(a.rgba)+",\"symbol\":";quote(output,a.symbol);output+='}';
            }output+="]}";
        }
        bool sonar_active=false;
        if(now<s.sonar_deadline)for(const auto& [owner,item]:s.owners){
            if(item.sonar.visible)sonar_active=true;
            if(item.annotations_visible&&item.annotations.version>=2&&item.annotations.context==s.map_context)
                for(unsigned i=0;i<item.annotations.count;++i)
                    if(!(item.annotations.items[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)){sonar_active=true;break;}
        }
        output+="],\"sonar_active\":"+std::string(sonar_active?"true":"false")+",\"editor_supported\":"+std::string(s.editor_supported?"true":"false")+",\"editing\":"+std::string(s.editor_deadline?"true":"false")+"}";return 200;
    }catch(...){output.clear();return 500;}
}
std::string ModDrawing::diagnostics() {
    auto& s=*impl_;std::lock_guard lock(s.mutex);const auto now=s.clock();s.expire(now);
    unsigned owners=0,native=0;for(const auto& [id,item]:s.owners)if(item.annotations_visible){++owners;native+=(item.annotations.flags&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS)?1:0;}
    std::string out="{\"schema\":1,\"reports\":"+std::to_string(s.ui_reports)+",\"fresh\":"+(s.ui_reports&&now>=s.ui_status_time&&now-s.ui_status_time<1500?std::string("true"):std::string("false"));
    out+=",\"map_fresh\":"+std::string(now<s.map_deadline?"true":"false")+",\"owners\":"+std::to_string(owners)+",\"native_owners\":"+std::to_string(native)+",\"ui\":[";
    for(size_t i=0;i<s.ui_status.size();++i){if(i)out+=',';out+=std::to_string(s.ui_status[i]);}return out+"]}";
}
ModDrawing& process_drawing(){static auto* service=new ModDrawing;return *service;}
}
