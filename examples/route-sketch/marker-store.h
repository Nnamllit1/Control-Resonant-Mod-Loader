/* Guest-owned marker metadata. Stored schemas 1/2 retain the legacy item wire
   layout: native slot in id, normalized map pixels in position[0..1], and the
   NATIVE_MARKER flag. Only the live dev.10 API uses typed world/attachment data. */
enum { SAVED_MARKERS = 128 };
typedef struct saved_marker { float geometry[8]; crml_map_annotation item; } saved_marker;
typedef struct marker_archive { uint32_t magic, version, count, reserved; saved_marker entries[SAVED_MARKERS]; } marker_archive;
_Static_assert(sizeof(marker_archive)<=65536,"archive exceeds storage limit");
static marker_archive archive;
static crml_map_projection current_projection;
static uint64_t store_generation, store_sent, store_next, store_now;
static int store_pending, store_enabled, store_dirty;
static uint64_t restored_context;
static unsigned restored_index;
static float restored_rect[4];
static int64_t store_notice_ticket;
static const char* store_notice_text;
static unsigned store_notice_level;
static uint64_t store_notice_next;
static void store_notice_tick(uint64_t now) {
    if(!store_notice_text||now<store_notice_next)return;
    if(store_notice_ticket>0){crml_feedback_dismiss((uint64_t)store_notice_ticket);store_notice_ticket=0;}
    unsigned length=0;while(store_notice_text[length])++length;
    const int64_t ticket=crml_feedback_show(store_notice_text,length,store_notice_level,2500);
    store_notice_next=now+1000;
    if(ticket>0){store_notice_ticket=ticket;store_notice_text=0;}
    else if(ticket!=-2&&ticket!=-4)store_notice_text=0;
}
static void store_notice(const char* message,unsigned level) {
    unsigned length=0;while(message[length])++length;crml_log(level==CRML_FEEDBACK_ERROR?3:1,message,length);
    // All callers supply string literals; retain the newest outcome until the
    // feedback service's per-owner rate limit allows it to be displayed.
    store_notice_text=message;store_notice_level=level;
    store_notice_tick(store_now);
}
static void store_invalidate(void) { restored_context=0; }
static void store_clear_markers(void) {
    if(!store_enabled)return;
    archive.count=0;
    ++store_generation;store_dirty=1;restored_index=0;
    store_next=store_now;
    store_notice("Route sketch marker details cleared. Saving...",CRML_FEEDBACK_INFO);
}
static int same_geometry(const float* a,const float* b) { for(unsigned i=0;i<8;++i)if(a[i]!=b[i])return 0;return 1; }
static int terminated(const char* s,unsigned n) { for(unsigned i=0;i<n;++i)if(!s[i])return 1;return 0; }
static void store_load(void) {
    store_enabled=(crml_capabilities()&CRML_CAP_STORAGE)!=0;if(!store_enabled)return;
    const int length=crml_storage_read(&archive,sizeof(archive));
    if(length==-2||length==0){archive.count=0;return;}
    int valid=length>=16&&archive.magic==0x4d4c5243u&&(archive.version==1||archive.version==2)&&archive.count<=SAVED_MARKERS&&
        length==(int)(16+archive.count*sizeof(saved_marker))&&archive.reserved==0;
    if(valid)for(unsigned i=0;i<archive.count;++i){const saved_marker* s=&archive.entries[i];const crml_map_annotation* a=&s->item;
        for(unsigned j=0;j<8;++j)if(!__builtin_isfinite(s->geometry[j]))valid=0;
        const int native=a->flags==3;
        if((!native&&(archive.version!=2||a->flags!=1||a->id<100||a->id>1000000))||
            (native&&(a->id<1||a->id>6||a->distance!=0||a->position[2]!=0||
            !(a->position[0]>=0&&a->position[0]<=1&&a->position[1]>=0&&a->position[1]<=1)))||
            !__builtin_isfinite(a->distance)||a->distance<0||a->distance>1000000||
            !__builtin_isfinite(a->position[0])||!__builtin_isfinite(a->position[1])||!__builtin_isfinite(a->position[2])||
            !terminated(a->name,64)||!terminated(a->description,128)||!terminated(a->symbol,8))valid=0;
    }
    for(unsigned i=0;valid&&i<archive.count;++i)if(!(archive.entries[i].item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)&&archive.entries[i].item.id>=next_marker_id)next_marker_id=archive.entries[i].item.id+1;
    if(!valid){archive.count=0;store_enabled=0;LOG("Saved marker details could not be read; leaving the stored file untouched.");}
}
static void store_tick(uint64_t now) {
    store_now=now;
    store_notice_tick(now);
    if(!store_enabled)return;
    if(store_pending){const int status=crml_storage_status();if(status==1)return;
        store_pending=0;
        if(status==2&&store_sent==store_generation){store_dirty=0;store_notice("Marker details saved.",CRML_FEEDBACK_SUCCESS);}
        if(status<0){store_notice("Marker details are not saved yet; retrying.",CRML_FEEDBACK_WARNING);store_next=now+2000;}
    }
    if(!store_dirty||now<store_next)return;
    // ModStorage already verifies checksums and atomically replaces its record.
    archive.magic=0x4d4c5243u;archive.version=2;archive.reserved=0;
    const int result=crml_storage_write(&archive,16+archive.count*sizeof(saved_marker));
    store_next=now+1000;
    if(result==0){store_pending=1;store_sent=store_generation;}
}
static void store_edit(const crml_map_annotation_event_v2* event) {
    if(!store_enabled)return;
    const int native=event->target==CRML_MAP_ANNOTATION_TARGET_NATIVE;
    crml_map_annotation item={0};
    if(native){
        const crml_map_native_attachment* a=&event->attachment;
        item.id=a->native_slot;item.flags=a->flags|CRML_MAP_ANNOTATION_NATIVE_MARKER;item.rgba=a->rgba;
        item.position[0]=a->map_pixels[0];item.position[1]=a->map_pixels[1];
        copy(item.symbol,a->symbol,sizeof(item.symbol));text(item.name,a->name);copy(item.description,a->description,sizeof(item.description));
    }else{
        const crml_map_world_annotation* a=&event->world;
        item.id=a->id;item.flags=a->flags;item.rgba=a->rgba;item.distance=a->distance;
        for(unsigned j=0;j<3;++j)item.position[j]=a->world_position[j];
        copy(item.symbol,a->symbol,sizeof(item.symbol));text(item.name,a->name);copy(item.description,a->description,sizeof(item.description));
    }
    crml_map_projection fresh;
    if(crml_map_projection_read(&fresh,sizeof(fresh))==1){
        if(native&&current_projection.context==fresh.context)for(unsigned j=0;j<4;++j)if(current_projection.rect[j]!=fresh.rect[j]){
            store_notice("Map resized. Reopen this marker and save again.",CRML_FEEDBACK_WARNING);return;}
        current_projection=fresh;
    }
    if(current_projection.context!=map_state.context||current_projection.rect[2]<=0||current_projection.rect[3]<=0){LOG("Marker changed, but map geometry is unavailable for saving.");return;}
    const float u=(item.position[0]-current_projection.rect[0])/current_projection.rect[2];
    const float v=(item.position[1]-current_projection.rect[1])/current_projection.rect[3];
    if(native&&!(u>=0&&u<=1&&v>=0&&v<=1)){store_notice("Marker position changed. Reopen it and save again.",CRML_FEEDBACK_WARNING);return;}
    unsigned index=archive.count;
    for(unsigned i=0;i<archive.count;++i)if(archive.entries[i].item.id==item.id&&
        !!(archive.entries[i].item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)==native&&
        same_geometry(archive.entries[i].geometry,current_projection.world_to_map)){index=i;break;}
    if(event->action==CRML_MAP_ANNOTATION_DELETE){
        if(index==archive.count)return;
        for(unsigned i=index+1;i<archive.count;++i)archive.entries[i-1]=archive.entries[i];--archive.count;
    }else{
        if(index==archive.count){if(archive.count==SAVED_MARKERS){LOG("Saved marker archive is full; this edit remains session-only.");return;}++archive.count;}
        saved_marker* s=&archive.entries[index];for(unsigned j=0;j<8;++j)s->geometry[j]=current_projection.world_to_map[j];
        s->item=item;
        if(native){s->item.position[0]=u;s->item.position[1]=v;}
    }
    ++store_generation;store_dirty=1;restored_index=0;
    store_notice("Marker updated. Saving details...",CRML_FEEDBACK_INFO);
    store_tick(store_now);
}
static void store_restore(void) {
    crml_map_projection fresh;if(crml_map_projection_read(&fresh,sizeof(fresh))!=1)return;
    current_projection=fresh;
    int changed=restored_context!=fresh.context;
    for(unsigned j=0;j<4;++j)if(restored_rect[j]!=fresh.rect[j])changed=1;
    if(changed)restored_index=0;
    restored_context=fresh.context;for(unsigned j=0;j<4;++j)restored_rect[j]=fresh.rect[j];
    if(!store_enabled)return;
    // Restore a bounded batch per tick; a full archive must fit the same fuel
    // allowance as a small mod. Edits mutate this archive before restoration.
    unsigned end=restored_index+8;if(end>archive.count)end=archive.count;
    for(unsigned i=restored_index;i<end;++i){const saved_marker* s=&archive.entries[i];
        if(!same_geometry(s->geometry,fresh.world_to_map))continue;
        unsigned at=marker_count;for(unsigned j=0;j<marker_count;++j)if(markers[j].id==s->item.id&&
            !!(markers[j].flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)==!!(s->item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER)){at=j;break;}
        if(at==marker_count){if(marker_count==MARKERS)continue;++marker_count;}
        marker* m=&markers[at];m->id=s->item.id;m->flags=s->item.flags&CRML_MAP_ANNOTATION_NATIVE_MARKER;m->distance=s->item.distance;m->saved=1;
        if(m->flags){m->position.p[0]=fresh.rect[0]+s->item.position[0]*fresh.rect[2];m->position.p[1]=fresh.rect[1]+s->item.position[1]*fresh.rect[3];m->position.p[2]=0;}
        else for(unsigned j=0;j<3;++j)m->position.p[j]=s->item.position[j];
        m->color=s->item.rgba;text(m->name,s->item.name);copy(m->symbol,s->item.symbol,8);copy(m->description,s->item.description,128);
        ++annotation_revision;
    }
    restored_index=end;
}
