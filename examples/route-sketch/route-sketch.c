#include "crml.h"

/* Route recording, marker metadata and persistence policy belong to this guest.
   Only native marker appearance is restored; world routes remain session-only. */
enum { POINTS = CRML_DRAWING_SEGMENTS_MAX + 1, MARKERS = CRML_MAP_ANNOTATIONS_V2_MAX, RECENT_POINTS = 24 };
typedef struct point { float p[3]; unsigned start; } point;
typedef struct marker { point position; char name[64]; uint32_t color, flags; uint64_t id; char symbol[8], description[128]; float distance; unsigned saved; } marker;
static point trail[POINTS];
static marker markers[MARKERS];
static uint32_t count, marker_count, handles[6], name_handle;
static uint64_t generation, sequence, last_sample, next_frame, map_context;
static uint64_t map_seen, map_since;
static crml_map_state map_state;
static int map_ready, map_continuous;
static int showing, ready, annotations_showing;
static int drawing_result;
static int interrupted, simplified;
static float height_origin[3],height_up[3];
static int height_known, annotations_native_only;
static uint32_t annotation_flags, annotation_denied;
static uint64_t native_marker_context;
static uint64_t next_marker_id=100, annotation_revision=1;
static uint32_t draft_color=0xFFE09AFF;
#define LOG(text) crml_log(1, text, sizeof(text)-1)

static void hide_annotations(void) { if(annotations_showing)crml_map_annotations_hide();annotations_showing=0;annotation_denied=0; }
static void store_invalidate(void);
static void clear(void) { store_invalidate(); if(marker_count)++annotation_revision;count=marker_count=0;generation=sequence=last_sample=map_context=map_seen=0;map_continuous=interrupted=simplified=0;crml_map_hide();crml_map_hide_target(CRML_MAP_SONAR);hide_annotations(); }
static void hide(void) { if(showing)crml_drawing_hide();showing=0; }
static void copy(char* out,const char* in,unsigned capacity) { unsigned i=0;for(;i+1<capacity&&in[i];++i)out[i]=in[i];out[i]=0; }
static void text(char* out,const char* in) { copy(out,in,64); }
static float dot(const float* a,const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static float relative_height(const float* p) {
    float delta[3];for(unsigned i=0;i<3;++i)delta[i]=p[i]-height_origin[i];
    return height_known?dot(delta,height_up):0;
}
static uint32_t route_color(unsigned i) {
    /* Keep the recent end brighter, without animating unchanged geometry. */
    float midpoint[3];for(unsigned j=0;j<3;++j)midpoint[j]=(trail[i-1].p[j]+trail[i].p[j])*.5f;
    const float height=relative_height(midpoint);
    const uint32_t rgb=height>2?0x7AE8AC00u:height< -2?0xD4A5FF00u:0x9ADAEA00u;
    return rgb + 208u + (count>1?47u*i/(count-1):47u);
}
static void marker_text(char* out,const marker* m) {
    text(out,m->name);
    if(!height_known)return;
    unsigned keep=0;while(m->name[keep]&&keep<54)++keep;
    while(keep&&((unsigned char)m->name[keep]&0xc0)==0x80)--keep;
    out[keep]=0;
    const float height=relative_height(m->position.p);
    float magnitude=height<0?-height:height;
    if(!(magnitude>=0))magnitude=0;
    if(magnitude>9999)magnitude=9999;
    unsigned value=(unsigned)magnitude;
    char digits[4];unsigned n=0;
    do{digits[n++]=(char)('0'+value%10);value/=10;}while(value&&n<4);
    unsigned at=0;while(out[at])++at;
    /* Map names can use 63 UTF-8 bytes; reserve nine for the height suffix. */
    out[at++]=' ';out[at++]='[';out[at++]=(height<=-1.f)?'-':'+';
    while(n)out[at++]=digits[--n];
    out[at++]='u';out[at++]=']';out[at]=0;
}
#include "marker-store.h"

static void clear_markers(void) {
    marker_count=0;++annotation_revision;
    store_clear_markers();
}

static void marker_edits(void) {
    crml_map_annotation_event_v2 event;
    if(crml_map_annotations_next_v2(&event,sizeof(event))!=1||event.revision!=annotation_revision)return;
    const int native=event.target==CRML_MAP_ANNOTATION_TARGET_NATIVE;
    if(!native&&event.target!=CRML_MAP_ANNOTATION_TARGET_WORLD)return;
    if((event.action==CRML_MAP_ANNOTATION_ATTACH&&!native)||(event.action==CRML_MAP_ANNOTATION_CREATE&&native))return;
    const uint64_t id=native?event.attachment.native_slot:event.world.id;
    unsigned index=marker_count;
    if(event.action==CRML_MAP_ANNOTATION_ATTACH){
        for(unsigned i=0;i<marker_count;++i)if(markers[i].id==id&&(markers[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)){index=i;break;}
        if(index==marker_count){if(marker_count>=MARKERS)return;++marker_count;}
        markers[index].id=id;markers[index].flags=CRML_MAP_ANNOTATION_NATIVE_MARKER;
        markers[index].position.p[0]=event.attachment.map_pixels[0];markers[index].position.p[1]=event.attachment.map_pixels[1];markers[index].position.p[2]=0;
    }else if(event.action==CRML_MAP_ANNOTATION_CREATE){
        if(marker_count>=MARKERS)return;
        markers[index].id=next_marker_id++;markers[index].flags=0;markers[index].saved=1;event.world.id=markers[index].id;
        for(unsigned j=0;j<3;++j)markers[index].position.p[j]=event.world.world_position[j];
        ++marker_count;
    }else{
        for(unsigned i=0;i<marker_count;++i)if(markers[i].id==id&&!!(markers[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)==native){index=i;break;}
        if(index==marker_count)return;
    }
    if(event.action==CRML_MAP_ANNOTATION_DELETE){
        for(unsigned i=index+1;i<marker_count;++i)markers[i-1]=markers[i];--marker_count;
    }else{
        if(native){
            text(markers[index].name,event.attachment.name);
            copy(markers[index].description,event.attachment.description,sizeof(markers[index].description));
            copy(markers[index].symbol,event.attachment.symbol,sizeof(markers[index].symbol));
            markers[index].color=event.attachment.rgba;markers[index].distance=0;
        }else{
            text(markers[index].name,event.world.name);
            copy(markers[index].description,event.world.description,sizeof(markers[index].description));
            copy(markers[index].symbol,event.world.symbol,sizeof(markers[index].symbol));
            markers[index].color=event.world.rgba;markers[index].distance=event.world.distance;
        }
    }
    if(event.action!=CRML_MAP_ANNOTATION_DELETE&&native)markers[index].saved=1;
    store_edit(&event);
    ++annotation_revision;
}
static int marker_visible(const marker* m) {
    if(m->flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)return 0; // Native UI owns placement; these are not world coordinates.
    float d=0;for(unsigned j=0;j<3;++j){const float v=m->position.p[j]-height_origin[j];d+=v*v;}
    return m->distance==0||d<=m->distance*m->distance;
}
static void publish_annotations(int native_only) {
    native_only=native_only||!height_known;
    if(native_only!=annotations_native_only){annotations_native_only=native_only;++annotation_revision;}
    static crml_map_annotations_v3 frame;frame.count=frame.attachment_count=0;for(unsigned j=0;j<3;++j){frame.origin[j]=0;frame.up[j]=0;}frame.version=3;frame.size=sizeof(frame);frame.context=map_state.context;
    frame.revision=annotation_revision;frame.flags=CRML_MAP_ANNOTATIONS_NATIVE_MARKERS;
    if(height_known&&map_ready)frame.flags|=CRML_MAP_ANNOTATIONS_PLACE_ACTION;
    if(height_known&&map_ready&&store_enabled&&archive.count<SAVED_MARKERS&&marker_count<MARKERS)frame.flags|=CRML_MAP_ANNOTATIONS_CREATE;
    if(annotation_flags!=frame.flags){annotation_flags=frame.flags;++annotation_revision;frame.revision=annotation_revision;}
    if(!height_known)frame.up[2]=1; // Unused plane; native attachments carry map pixels.
    else for(unsigned j=0;j<3;++j){frame.origin[j]=height_origin[j];frame.up[j]=height_up[j];}
    for(unsigned i=0;i<marker_count;++i){
        const marker* m=&markers[i];if(native_only&&!m->saved&&!(m->flags&CRML_MAP_ANNOTATION_NATIVE_MARKER))continue;
        if(m->flags&CRML_MAP_ANNOTATION_NATIVE_MARKER){
            if(frame.attachment_count>=CRML_MAP_NATIVE_ATTACHMENTS_MAX)continue;
            crml_map_native_attachment* a=&frame.attachments[frame.attachment_count++];
            a->native_slot=(uint32_t)m->id;a->flags=CRML_MAP_ANNOTATION_EDITABLE;a->rgba=m->color;a->reserved=0;
            a->map_pixels[0]=m->position.p[0];a->map_pixels[1]=m->position.p[1];
            text(a->name,m->name);copy(a->description,m->description,sizeof(a->description));copy(a->symbol,m->symbol,sizeof(a->symbol));
        }else{
            crml_map_world_annotation* a=&frame.items[frame.count++];
            a->id=m->id;a->rgba=m->color;a->flags=CRML_MAP_ANNOTATION_EDITABLE;a->distance=m->distance;
            for(unsigned j=0;j<3;++j)a->world_position[j]=m->position.p[j];
            text(a->name,m->name);copy(a->description,m->description,sizeof(a->description));copy(a->symbol,m->symbol,sizeof(a->symbol));
        }
    }
    if(crml_map_annotations_publish_v3(&frame,sizeof(frame))==1){
        annotations_showing=1;
        crml_map_annotation_status status;
        if(crml_map_annotations_status(&status,sizeof(status))==1){
            const uint32_t denied=status.requested&~status.granted;
            if((denied&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS)&&!(annotation_denied&CRML_MAP_ANNOTATIONS_NATIVE_MARKERS))
                store_notice("Stock marker editing is busy; retry when available.",CRML_FEEDBACK_WARNING);
            if((denied&CRML_MAP_ANNOTATIONS_PLACE_ACTION)&&!(annotation_denied&CRML_MAP_ANNOTATIONS_PLACE_ACTION))
                store_notice("Extra marker placement is busy; retry when available.",CRML_FEEDBACK_WARNING);
            annotation_denied=denied;
        }
    }
}
static void observe_map(uint64_t now) {
    const int previously_ready=map_ready;
    const uint64_t previous_context=map_state.context;
    crml_map_state observed;
    map_ready=crml_map_read(&observed,sizeof(observed))==1;
    if(map_ready)map_state=observed;
    map_continuous=map_ready&&map_context==map_state.context&&now>=map_seen&&now-map_seen<=500;
    if(map_ready&&(!previously_ready||previous_context!=map_state.context||now<map_seen||now-map_seen>500))map_since=now;
    if(map_ready&&native_marker_context!=map_state.context){
        unsigned kept=0;for(unsigned i=0;i<marker_count;++i)if(!markers[i].saved&&!(markers[i].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER))markers[kept++]=markers[i];
        if(kept!=marker_count){marker_count=kept;++annotation_revision;}native_marker_context=map_state.context;
    }
    if(map_ready){store_restore();map_seen=now;}
    else map_seen=0;
}
static int extends_straight(const point* before,const point* last,const float* next) {
    float a[3],b[3];
    for(unsigned i=0;i<3;++i){a[i]=last->p[i]-before->p[i];b[i]=next[i]-last->p[i];}
    const float aa=dot(a,a),ab=dot(a,b);
    /* Only merge forward, nearly collinear segments; retain corners/reversals. */
    return aa>.0001f&&ab>0&&dot(b,b)-ab*ab/aa<=.000001f;
}
/* An online overview, not a sliding window: retain the route's start and recent
   samples, reducing the least significant older bend when the draw budget fills.
   Never simplify across an unobserved interval or connect two separate runs. */
static void make_room(void) {
    if(count<POINTS)return;
    unsigned remove=0;float best=3.402823466e38f;
    for(unsigned i=1;i+RECENT_POINTS<count;++i) {
        if(trail[i].start||trail[i+1].start)continue;
        float a[3],b[3];
        for(unsigned j=0;j<3;++j){a[j]=trail[i+1].p[j]-trail[i-1].p[j];b[j]=trail[i].p[j]-trail[i-1].p[j];}
        const float aa=dot(a,a);
        float t=aa>.000001f?dot(a,b)/aa:0;
        if(t<0)t=0;else if(t>1)t=1;
        for(unsigned j=0;j<3;++j)b[j]-=t*a[j];
        const float error=dot(b,b);
        if(error<best){best=error;remove=i;}
    }
    if(remove) {
        for(unsigned i=remove+1;i<count;++i)trail[i-1]=trail[i];
        --count;
        if(!simplified){LOG("Route overview budget reached; simplifying older bends, keeping the start and recent detail.");simplified=1;}
    } else {
        /* Too many separate tiny runs to simplify. Evict a whole oldest run,
           never invent a connector across a gap. This bound is explicit. */
        unsigned end=1;while(end<count&&!trail[end].start)++end;
        for(unsigned i=end;i<count;++i)trail[i-end]=trail[i];
        count-=end;
        LOG("Route run capacity reached; discarded the oldest disconnected section.");
    }
}
static void cross(const float* a,const float* b,float* c) {
    c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];
}
static int project(const point* p,const crml_navigation_state_v2* current,const float* right,const float* forward,float* x,float* y) {
    float offset[3];for(unsigned i=0;i<3;++i)offset[i]=p->p[i]-current->position[i];
    *x=.5f+dot(offset,right)/80.f;*y=.5f-dot(offset,forward)/80.f;
    return *x>=.04f&&*x<=.96f&&*y>=.12f&&*y<=.9f;
}
static int clip_axis(float p,float d,float low,float high,float* begin,float* end) {
    if(d==0)return p>=low&&p<=high;
    float a=(low-p)/d,b=(high-p)/d;
    if(a>b){const float swap=a;a=b;b=swap;}
    if(a>*begin)*begin=a;if(b<*end)*end=b;
    return *begin<=*end;
}
static int clip_line(crml_drawing_segment* line) {
    const float dx=line->x2-line->x1,dy=line->y2-line->y1;
    float begin=0,end=1;
    if(!clip_axis(line->x1,dx,.04f,.96f,&begin,&end)||!clip_axis(line->y1,dy,.12f,.9f,&begin,&end))return 0;
    line->x2=line->x1+end*dx;line->y2=line->y1+end*dy;
    line->x1+=begin*dx;line->y1+=begin*dy;return 1;
}
static void render(const crml_navigation_state_v2* current) {
    crml_drawing_frame frame={0};frame.version=1;frame.size=sizeof(frame);frame.lifetime_ms=600;
    frame.x=.04f;frame.y=.24f;frame.width=.26f;frame.height=.42f;
    float axis[3]={1,0,0},right[3],forward[3];
    if(current->up[0]>.9f||current->up[0]<-.9f){axis[0]=0;axis[2]=1;}
    cross(current->up,axis,right);
    const float inverse=1.f/__builtin_sqrtf(dot(right,right));
    for(unsigned i=0;i<3;++i)right[i]*=inverse;
    cross(right,current->up,forward);
    for(unsigned i=1;i<count;++i) {
        if(trail[i].start)continue;
        crml_drawing_segment line={0};
        project(&trail[i-1],current,right,forward,&line.x1,&line.y1);
        project(&trail[i],current,right,forward,&line.x2,&line.y2);
        if(clip_line(&line)) {
            line.width_vh=.20f;line.rgba=route_color(i);frame.segments[frame.segment_count++]=line;
        }
    }
    crml_drawing_label* label=&frame.labels[frame.label_count++];
    label->x=.04f;label->y=.02f;label->font_vh=1.6f;label->rgba=0xFFFFFFFF;text(label->text,"Session route / movement plane");
    label=&frame.labels[frame.label_count++];label->x=.5f;label->y=.5f;label->font_vh=1.6f;label->rgba=0xFFFFFFFF;text(label->text,"+");
    for(unsigned i=0;i<marker_count;++i) {
        if(!marker_visible(&markers[i])||frame.label_count>=CRML_DRAWING_LABELS_MAX-1)continue;
        float x,y;if(!project(&markers[i].position,current,right,forward,&x,&y))continue;
        label=&frame.labels[frame.label_count++];label->x=x;label->y=y;label->font_vh=1.6f;label->rgba=markers[i].color;marker_text(label->text,&markers[i]);
    }
    label=&frame.labels[frame.label_count++];label->x=.04f;label->y=.94f;label->font_vh=1.3f;label->rgba=0xD5DCE3FF;text(label->text,height_known?"Green above / cyan level / purple below":"Height unavailable");
    const int result=crml_drawing_publish(&frame,sizeof(frame));
    if(result!=drawing_result) {
        if(result==1&&drawing_result<0)LOG("Route drawing resumed.");
        else if(result==-1)LOG("Route drawing unavailable; retrying while enabled.");
        else if(result==-2)LOG("Route drawing capacity occupied; retrying while enabled.");
        else if(result<0)LOG("Route drawing rejected; retrying while enabled.");
        drawing_result=result;
    }
    showing=result==1||showing;
}
static void world_frame(crml_map_frame* frame) {
    for(unsigned i=1;i<count;++i){
        if(trail[i].start)continue;
        crml_map_segment* line=&frame->segments[frame->segment_count++];
        for(unsigned j=0;j<3;++j){line->from[j]=trail[i-1].p[j];line->to[j]=trail[i].p[j];}
        line->width_vh=.20f;line->rgba=route_color(i);
    }
    for(unsigned i=0;i<marker_count;++i){
        if(frame->target==CRML_MAP_FULL||frame->target==CRML_MAP_SONAR||!marker_visible(&markers[i])||frame->label_count>=CRML_DRAWING_LABELS_MAX)continue;
        crml_map_label* label=&frame->labels[frame->label_count++];
        for(unsigned j=0;j<3;++j)label->position[j]=markers[i].position.p[j];
        label->font_vh=1.6f;label->rgba=markers[i].color;marker_text(label->text,&markers[i]);
    }
}
static int render_sonar(uint64_t now) {
    crml_map_state state;
    if(!count||now<last_sample||now-last_sample>500||crml_map_read_target(CRML_MAP_SONAR,&state,sizeof(state))!=1){
        crml_map_hide_target(CRML_MAP_SONAR);return 0;
    }
    crml_map_frame frame={0};frame.version=CRML_MAP_VERSION;frame.size=sizeof(frame);
    frame.target=CRML_MAP_SONAR;frame.context=state.context;frame.lifetime_ms=600;
    world_frame(&frame);
    for(unsigned i=0;i<frame.segment_count;++i)frame.segments[i].width_vh=.15f;
    for(unsigned i=0;i<frame.label_count;++i)frame.labels[i].font_vh=1.1f;
    if(!frame.segment_count&&!frame.label_count){crml_map_hide_target(CRML_MAP_SONAR);return 1;}
    return crml_map_publish(&frame,sizeof(frame))>=0;
}
static void render_map(uint64_t now) {
    const int route_ready=count&&map_ready&&now>=last_sample&&
        (now-last_sample<=1000||map_continuous)&&(map_continuous||last_sample>=map_since);
    crml_map_state sonar;
    if(map_state.context&&(map_ready||crml_map_read_target(CRML_MAP_SONAR,&sonar,sizeof(sonar))==1))
        publish_annotations(!route_ready);
    else hide_annotations();
    // Native slot details are independent of route samples and the height plane.
    // World route/annotations still wait for current navigation evidence.
    if(!route_ready){crml_map_hide();return;}
    map_context=map_state.context;
    crml_map_frame frame={0};frame.version=CRML_MAP_VERSION;frame.size=sizeof(frame);
    frame.target=CRML_MAP_FULL;frame.context=map_state.context;frame.lifetime_ms=600;
    world_frame(&frame);
    if(frame.segment_count||frame.label_count)crml_map_publish(&frame,sizeof(frame));
    else crml_map_hide();
}
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {
    store_load();
    const crml_setting_definition definitions[]={
        {1,CRML_SETTING_BOOL,"visible","Show route","Green above, cyan level, purple below your current ground plane. Marker +/- values are world units. Hiding clears the route.",1,0,1,1},
        {1,CRML_SETTING_BOOL,"clear","Clear route","Clear the trail and temporary markers. Saved map markers remain.",0,0,1,1},
        {1,CRML_SETTING_BOOL,"mark","Place named marker","Shortcut: place a yellow marker at the player. Use the full map to place and edit markers directly.",0,0,1,1},
        {1,CRML_SETTING_BOOL,"confirm_markers","Confirm marker deletion","Enable before Delete Route sketch markers. This removes this mod's markers and saved details across all map geometries.",0,0,1,1},
        {1,CRML_SETTING_BOOL,"delete_markers","Delete Route sketch markers","Deletes this mod's world markers and stock X appearance details after confirmation. Stock X markers remain in the game.",0,0,1,1},
    };
    ready=1;
    for(unsigned i=0;i<5;++i){const int h=crml_settings_register(&definitions[i],sizeof(definitions[i]));if(h<1)ready=0;else handles[i]=(uint32_t)h;}
    const crml_text_setting_definition name={1,48,"marker_name","Marker name","Name for the player-position shortcut. Map markers can be named directly on the full map.","Return here"};
    const int h=crml_settings_text_register(&name,sizeof(name));if(h<1)ready=0;else handles[5]=name_handle=(uint32_t)h;
    LOG("Route sketch: session routes; saved native marker details use matching map geometry and position, not save identity.");
}
void crml_tick(float dt) {
    (void)dt;
    if(!ready)return;
    const uint64_t now=crml_clock_ms();
    store_tick(now);
    crml_setting_value settings[6];
    if(crml_settings_read(settings,sizeof(settings))!=6){hide();clear();return;}
    for(unsigned i=0;i<6;++i)if(settings[i].handle!=handles[i]){hide();clear();return;}
    if(settings[1].value) {
        clear();crml_settings_set(handles[1],0,settings[1].revision);LOG("Route sketch cleared.");
    }
    if(settings[4].value){
        if(settings[3].value){
            clear_markers();crml_settings_set(handles[3],0,settings[3].revision);
            LOG("Route sketch markers and saved appearance details cleared.");
        }else LOG("Marker deletion requires the confirmation setting.");
        crml_settings_set(handles[4],0,settings[4].revision);
    }
    if(!settings[0].value){hide();clear();return;}
    marker_edits();
    if(now<next_frame&&now>=last_sample)return;
    next_frame=now+200;
    observe_map(now);
    crml_navigation_state_v2 current;
    if(crml_navigation_read_v2(&current,sizeof(current))!=1) {
        interrupted=1;
        hide();crml_map_hide_target(CRML_MAP_SONAR);render_map(now);return;
    }
    const int gap=generation&&(now<last_sample||now-last_sample>500);
    if((generation&&generation!=current.continuity)||(current.flags&CRML_NAV_TELEPORTED)||now<last_sample) {
        if(count) {
            if(generation!=current.continuity)LOG("Player continuity changed; starting a new route.");
            else if(current.flags&CRML_NAV_TELEPORTED)LOG("Player teleport observed; starting a new route.");
            else LOG("Clock moved backwards; starting a new route.");
        }
        if(generation||count||marker_count)clear();
    }
    else if(gap||interrupted) {
        interrupted=1;
    }
    generation=current.continuity;last_sample=now>=current.age_ms?now-current.age_ms:0;
    height_known=(current.flags&CRML_NAV_UP_VALID)!=0;
    for(unsigned i=0;i<3;++i){height_origin[i]=current.position[i];height_up[i]=current.up[i];}
    if(current.flags&(CRML_NAV_CONTROLLER_DISABLED|CRML_NAV_KEYFRAMED)) {interrupted=1;hide();crml_map_hide_target(CRML_MAP_SONAR);render_map(now);return;}
    if(sequence!=current.sequence) {
        float distance=0;for(unsigned i=0;i<3;++i){const float d=count?current.position[i]-trail[count-1].p[i]:0;distance+=d*d;}
        if(distance>400&&!interrupted){LOG("Position jump observed; starting a new route.");clear();generation=current.continuity;last_sample=now>=current.age_ms?now-current.age_ms:0;}
        if(!count||interrupted||distance>=.25f) {
            if(count&&interrupted)LOG("Player observations resumed in the same lifetime; retaining the route with a gap.");
            if(!interrupted&&count>=2&&!trail[count-1].start&&extends_straight(&trail[count-2],&trail[count-1],current.position)) {
                for(unsigned i=0;i<3;++i)trail[count-1].p[i]=current.position[i];
            } else {
                make_room();
                for(unsigned i=0;i<3;++i)trail[count].p[i]=current.position[i];
                trail[count].start=!count||interrupted;++count;
            }
            interrupted=0;
        }
        sequence=current.sequence;
    }
    if(settings[2].value) {
        crml_text_setting_value name;
        if(crml_settings_text_read(name_handle,&name,sizeof(name))==1) {
            if(name.length&&marker_count<MARKERS){
                for(unsigned i=0;i<3;++i)markers[marker_count].position.p[i]=current.position[i];
                text(markers[marker_count].name,name.value);markers[marker_count].color=draft_color;
                markers[marker_count].flags=0;markers[marker_count].saved=0;markers[marker_count].id=next_marker_id++;copy(markers[marker_count].symbol,"*",8);markers[marker_count].description[0]=0;markers[marker_count].distance=0;
                ++marker_count;++annotation_revision;LOG("Temporary marker placed.");
            } else {LOG("Marker not placed: name is empty or all 128 slots are occupied.");}
            crml_settings_set(handles[2],0,settings[2].revision);
        }
    }
    if(render_sonar(now))hide();
    else if(current.flags&CRML_NAV_UP_VALID)render(&current);else hide();
    render_map(now);
}
void crml_shutdown(void) { store_tick(crml_clock_ms()+1000);crml_drawing_hide();crml_map_hide();crml_map_hide_target(CRML_MAP_SONAR);crml_map_annotations_hide(); }
