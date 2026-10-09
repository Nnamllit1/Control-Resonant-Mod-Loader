#include "mod_drawing.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
crml_drawing_frame frame() {
    crml_drawing_frame f{};f.version=CRML_DRAWING_VERSION;f.size=sizeof(f);f.lifetime_ms=1000;
    f.width=.5f;f.height=.5f;f.segment_count=2;f.label_count=1;
    f.segments[0]={.1f,.1f,.4f,.4f,.2f,0xffaa00ff};
    f.segments[1]={.6f,.6f,.9f,.9f,.2f,0xffaa00ff};
    f.labels[0]={.1f,.2f,2,0xffffffff,{}};strcpy_s(f.labels[0].text,"Marker <1> \"north\"");return f;
}
void expanded_annotation_tests() {
    uint64_t now=100;crml::ModDrawing service([&]{return now;});service.attach(1,"expanded");service.enable_renderer(true);auto page=service.open_page();
    crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};service.update_map(d,{0,0,400,300});
    crml_map_state state{};require(service.map_read(1,state)==1,"expanded map ready");
    crml_map_annotations_v2 f{};f.version=2;f.size=sizeof(f);f.context=state.context;f.revision=1;f.flags=7;f.up[2]=1;
    require(service.annotations_publish_v2(1,f)==1,"expanded owner admission");
    require(!service.placement_action({200,150},false),"free stock slots keep native placement");
    require(service.placement_action({200,150},true),"stock overflow queues guest placement");
    require(service.placement_action({200,150},false),"pending identical action cannot accidentally create a stock duplicate");
    require(!service.placement_action({210,150},true),"pending action cannot duplicate placement");
    crml_map_annotation_event event{};require(service.annotations_next(1,event)==1&&event.action==1&&event.item.id==0,"placement is a generic create event");
    require(std::abs(event.item.position[0]-50)<.01f&&std::abs(event.item.position[1]-100)<.01f,"native map pixels unproject on explicit plane");
    now+=100;f.count=128;f.revision=2;
    for(unsigned i=0;i<f.count;++i){auto& a=f.items[i];a.id=100+i;a.position[0]=float(i%100);a.position[1]=100;a.flags=1;a.rgba=0xffffffff;std::strcpy(a.symbol,"*");std::strcpy(a.name,"Extended marker");}
    require(service.annotations_publish_v2(1,f)==1,"all 128 annotations accepted");
    std::string body;require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/1",body)==200&&body.find("\"id\":\"227\"")!=body.npos,"last expanded marker serialized");
    require(!service.placement_action({390,280},true),"capacity refuses additional creation");
    require(service.placement_action({0,150},true)&&service.annotations_next(1,event)==1&&event.action==3&&event.item.id==100,"X toggles an extended marker even at capacity");
    now+=100;f.count=1;f.revision=3;f.items[0].distance=1;
    require(service.annotations_publish_v2(1,f)==1,"range-filtered marker admitted");
    require(!service.placement_action({0,150},false),"invisible marker cannot consume X or delete unseen data");
    now+=100;f.count=129;require(service.annotations_publish_v2(1,f)==-3,"v2 overflow rejected before item access");
    service.annotations_hide(1);require(!service.placement_action({200,150},true),"hidden owner never consumes native placement");
}
void map_tests() {
    uint64_t now=100;crml::ModDrawing service([&]{return now;});service.attach(1,"map-owner");service.enable_renderer(true);
    auto page=service.open_page();crml_map_state state{};
    require(service.map_read(1,state)==-1&&state.context==0,"no fabricated native context");
    crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};
    service.update_map(d,{10,20,400,300});require(service.map_read(1,state)==1&&state.context,"fresh copied projection");
    crml_map_frame f{};f.version=1;f.size=sizeof(f);f.target=1;f.context=state.context;f.lifetime_ms=600;f.segment_count=2;
    f.segments[0]={{-100,100,0},{200,100,0},.1f,0xffffffff};
    f.segments[1]={{-100,-20,0},{200,-20,0},.1f,0xffffffff};
    require(service.map_publish(1,f)==1,"world-space frame admitted");
    std::vector<crml::ModDrawing::Surface> rows;require(service.poll(page,1,rows)==200&&rows.size()==1,"native layer snapshot");
    const auto& frame=rows[0].frame;
    require(rows[0].target==1&&frame.x==10&&frame.width==400&&frame.segment_count==1,"native rectangle and geometric clipping");
    require(frame.segments[0].x1==0&&frame.segments[0].x2==1&&frame.segments[0].y1==.5f,"crossing segment clipped instead of dropped or clamped into border trail");
    crml_map_projection copied{};require(service.projection_read(1,copied)==1&&copied.version==1&&copied.size==64&&copied.context==state.context,"projection descriptor copied");
    require(std::abs(copied.world_to_map[0]-.01f)<.000001f&&std::abs(copied.world_to_map[5]+.005f)<.000001f&&copied.rect[2]==400,"affine rows expose native normalized coordinates");
    require(service.projection_read(99,copied)==-1&&copied.context==0,"projection owner isolation and zero output");
    const auto token=state.context;now+=100;service.update_map(d,{10,20,400,300});
    require(service.map_read(1,state)==1&&state.context==token,"unchanged projection identity preserved");
    auto bad=f;bad.segments[1].to[0]=std::numeric_limits<float>::infinity();require(service.map_publish(1,bad)==-3,"invalid coordinate cannot replace layer");
    service.update_map(d,{10,20,800,300});require(service.map_read(1,state)==1&&state.context==token,"native layout resize must not reset world route context");
    require(service.map_publish(1,f)==1,"layout resize accepts existing world coordinates");
    d.maximum[0]=200;service.update_map(d,{10,20,800,300});require(service.map_publish(1,f)==-6,"stale frame rejected after projection change");
    require(service.poll(page,2,rows)==200&&rows.empty(),"projection change retires prior geometry");
    require(service.map_read(1,state)==1&&state.context!=token,"new context observed");f.context=state.context;
    now+=100;require(service.map_publish(1,f)==1,"new projection frame admitted");
    const auto retained_context=state.context;
    now+=501;require(service.map_read(1,state)==-1&&state.context==0,"no native refresh means unavailable");
    require(service.projection_read(1,copied)==-1&&copied.context==0,"stale affine projection unavailable");
    require(service.poll(page,3,rows)==200&&rows.empty(),"expired projection removes map pixels before frame lease");
    service.update_map(d,{10,20,800,300});service.map_read(1,state);require(state.context==retained_context,"same map geometry survives closing and reopening; freshness remains separate");f.context=state.context;
    require(service.map_publish(1,f)==1,"fresh context restores admission");service.cancel(1);
    require(service.poll(page,4,rows)==200&&rows.empty(),"owner release clears map layer");
    service.enable_renderer(false);require(service.map_read(1,state)==-1,"renderer teardown unavailable");
}
void sonar_tests() {
    uint64_t now=100;crml::ModDrawing service([&]{return now;});
    for(uint64_t i=1;i<=4;++i)service.attach(i,"sonar-"+std::to_string(i));
    service.enable_renderer(true);const auto page=service.open_page();
    crml::sonar_projection::Snapshot snapshot{};
    snapshot.current.plane={0,0,0,1};snapshot.current.reference={0,0,0,1};
    snapshot.current.near_radius=20;snapshot.current.far_radius=80;snapshot.previous=snapshot.current;snapshot.interpolation=1;
    service.update_sonar(snapshot);crml_map_state state{};
    require(service.map_read_target(1,CRML_MAP_SONAR,state)==1&&state.target==2,"sonar destination independently available");
    const auto token=state.context;
    crml_map_frame f{};f.version=1;f.size=sizeof(f);f.target=2;f.context=token;f.lifetime_ms=600;
    f.segment_count=2;f.segments[0]={{-40,0,0},{40,0,0},.1f,0xffffffff};
    f.segments[1]={{-40,0,25},{40,0,25},.1f,0xffffffff};
    f.label_count=2;f.labels[0]={{0,4,0},1,0xffffffff,"Inside"};f.labels[1]={{21,0,0},1,0xffffffff,"Outside"};
    require(service.map_publish(1,f)==1,"sonar frame admitted");
    std::vector<crml::ModDrawing::Surface> rows;service.poll(page,1,rows);
    require(rows.size()==1&&rows[0].target==2&&rows[0].frame.width==1&&rows[0].frame.segment_count==1&&rows[0].frame.label_count==1,"near ring clips lines and drops distant labels");
    const auto& segment=rows[0].frame.segments[0];
    require(std::abs(segment.x1-.05f)<.001f&&std::abs(segment.x2-.95f)<.001f,"crossing route clips geometrically rather than creating a rim trail");
    require(service.map_hide(1)==0,"legacy map hide cannot erase minimap");
    require(service.map_read_target(1,3,state)==-3&&state.context==0,"unknown target clears read output");
    require(service.map_hide_target(1,3)==-3,"unknown hide target rejected");
    now+=100;snapshot.current.origin[0]=1;service.update_sonar(snapshot);
    require(service.map_read_target(1,2,state)==1&&state.context==token,"camera/player motion does not reset stream identity");
    auto invalid=f;invalid.context=token+1;require(service.map_publish(1,invalid)==-6,"sonar stale context rejected");
    require(service.publish(1,frame())==1&&service.publish(2,frame())==1&&service.publish(3,frame())==1,"native and schematic share slots");
    require(service.publish(4,frame())==-2,"sonar included in shared capacity");
    service.cancel(1);service.poll(page,2,rows);require(rows.size()==2,"release clears all owner destinations");
    require(service.map_publish(1,f)==1,"sonar can be republished after release");
    now+=501;require(service.map_read_target(1,2,state)==-1&&state.context==0,"stale sonar clears read output");
    service.poll(page,3,rows);require(std::none_of(rows.begin(),rows.end(),[](const auto& row){return row.target==2;}),"native stream expiry clears sonar");
    service.update_sonar(snapshot);require(service.map_read_target(1,2,state)==1&&state.context!=token,"reacquisition gets new stream identity");
    require(service.map_publish(1,f)==-6,"old coordinates require explicit fresh context");
    f.context=state.context;require(service.map_publish(1,f)==1,"fresh sonar accepted");
    service.detach(1);service.poll(page,4,rows);require(std::none_of(rows.begin(),rows.end(),[](const auto& row){return row.target==2;}),"unload clears sonar");
}
void sonar_reprojection_tests() {
    uint64_t now=100,sequence=0;crml::ModDrawing service([&]{return now;});
    for(uint64_t i=1;i<=5;++i)service.attach(i,"live-"+std::to_string(i));
    service.enable_renderer(true);const auto page=service.open_page();
    crml::sonar_projection::Snapshot snapshot{};snapshot.current.plane={0,0,0,1};snapshot.current.reference={0,0,0,1};
    snapshot.current.near_radius=20;snapshot.current.far_radius=80;snapshot.previous=snapshot.current;snapshot.interpolation=1;
    service.update_sonar(snapshot);crml_map_state state{};service.map_read_target(1,2,state);
    crml_map_frame f{};f.version=1;f.size=sizeof(f);f.target=2;f.context=state.context;f.lifetime_ms=600;
    f.segment_count=1;f.segments[0]={{0,0,0},{4,0,0},.1f,0xffffffff};
    f.label_count=1;f.labels[0]={{2,2,0},1,0xffffffff,"Position"};
    require(service.map_publish(1,f)==1,"live projection initial publication");
    std::vector<crml::ModDrawing::Surface> rows;service.poll(page,++sequence,rows);
    const auto first=rows.at(0);now+=16;snapshot.current.origin[0]=1;service.update_sonar(snapshot);
    service.poll(page,++sequence,rows);
    require(rows.size()==1&&rows[0].revision!=first.revision&&rows[0].frame.segments[0].x1!=first.frame.segments[0].x1&&
            rows[0].frame.labels[0].x!=first.frame.labels[0].x,"camera updates lines and labels without guest republish");
    require(rows[0].remaining_ms==500,"native freshness still bounds projected lease");
    const auto revision=rows[0].revision;service.poll(page,++sequence,rows);
    require(rows[0].revision==revision,"unchanged projection retains revision");
    require(service.map_publish(1,f)==-4,"projection refresh does not bypass guest rate limit");
    snapshot.current.origin[0]=100;service.update_sonar(snapshot);service.poll(page,++sequence,rows);
    require(rows.empty(),"camera can clip all world geometry");
    std::string body;auto exchange=[&]{return service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++sequence),body);};
    require(exchange()==200&&body.find("\"sonar_active\":true")!=body.npos,"offscreen source keeps renderer cadence active");
    snapshot.current.origin[0]=0;service.update_sonar(snapshot);service.poll(page,++sequence,rows);
    require(rows.size()==1,"camera can bring retained geometry back before lease ends");
    service.map_hide_target(1,2);service.update_sonar(snapshot);service.poll(page,++sequence,rows);
    require(rows.empty(),"hidden world source is not resurrected by camera movement");
    now=200;snapshot.current.origin[0]=100;service.update_sonar(snapshot);
    for(uint64_t i=1;i<=4;++i)require(service.map_publish(i,f)==0,"offscreen frame admitted with zero pixels");
    require(service.map_publish(5,f)==-2,"offscreen leases still respect shared surface capacity");
    now=300;snapshot.current.origin[0]=0;service.update_sonar(snapshot);service.poll(page,++sequence,rows);
    require(rows.size()==4,"initially offscreen frames reproject when camera moves");
    now=700;service.update_sonar(snapshot);now=800;service.update_sonar(snapshot);service.poll(page,++sequence,rows);
    require(rows.empty(),"live native snapshots do not prolong expired mod geometry");
    require(exchange()==200&&body.find("\"sonar_active\":false")!=body.npos,"expired sources release fast polling");
    crml::map_projection::District district{};district.rotation={0,0,0,1};district.maximum={100,100,100};district.scale={1,1};
    service.update_map(district,{0,0,400,300});service.map_read(1,state);
    crml_map_annotations_v2 annotations{};annotations.version=2;annotations.size=sizeof(annotations);annotations.context=state.context;
    annotations.revision=1;annotations.up[2]=1;annotations.count=1;
    auto& marker=annotations.items[0];marker.id=7;marker.position[0]=100;marker.rgba=0xffffffff;
    strcpy_s(marker.symbol,"A");strcpy_s(marker.name,"Offscreen");
    require(service.annotations_publish_v2(1,annotations)==1,"offscreen annotation source admitted");
    require(exchange()==200&&body.find("\"sonar_active\":true")!=body.npos,"annotation-only offscreen source keeps fast polling");
    service.annotations_hide(1);
    require(exchange()==200&&body.find("\"sonar_active\":false")!=body.npos,"hidden annotation source releases fast polling");
}
void annotation_v3_tests() {
    uint64_t now=100,seq=0;crml::ModDrawing service([&]{return now;});
    service.attach(90,"first");service.attach(1,"second");service.attach(50,"third");service.enable_renderer(true);
    const auto page=service.open_page();crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};
    service.update_map(d,{0,0,400,300});crml_map_state map{};service.map_read(90,map);
    crml_map_annotations_v3 f{};f.version=3;f.size=sizeof(f);f.context=map.context;f.revision=1;f.flags=7;f.up[2]=1;
    f.count=1;auto& world=f.items[0];world.id=2;world.world_position[0]=20;world.world_position[1]=100;world.flags=1;world.rgba=0xffffffff;strcpy_s(world.symbol,"W");strcpy_s(world.name,"World two");
    f.attachment_count=1;auto& attachment=f.attachments[0];attachment.native_slot=2;attachment.flags=1;attachment.rgba=0xffffffff;
    attachment.map_pixels[0]=200;attachment.map_pixels[1]=150;strcpy_s(attachment.symbol,"N");strcpy_s(attachment.name,"Native two");
    require(service.annotations_publish_v3(90,f)==1,"world ID2 and native slot2 coexist");
    crml_map_annotation_status status{};require(service.annotations_status(90,status)==1&&status.requested==6&&status.granted==6,"first arrival owns both interactions");
    require(service.annotations_publish_v3(1,f)==1&&service.annotations_publish_v3(50,f)==1,"waiting owners can publish independent annotation data");
    require(service.annotations_status(1,status)==1&&status.requested==6&&status.granted==0,"smaller numeric handle cannot steal ownership");
    std::string body;
    auto edit=[&](uint64_t owner,unsigned action,unsigned target){return service.exchange(std::string(crml::drawing_prefix)+"edit2/"+std::to_string(page)+"/"+std::to_string(++seq)+"/"+std::to_string(owner)+"/1/"+std::to_string(map.context)+"/"+std::to_string(action)+"/2/4294967295/0/0.5/0.5/t41/t456469746564/t/"+std::to_string(target),body);};
    require(edit(90,2,1)==202,"typed world edit accepted");
    crml_map_annotation_event legacy{};require(service.annotations_next(90,legacy)==-3&&legacy.version==0,"legacy reader cannot misinterpret or consume typed event");
    crml_map_annotation_event_v2 event{};require(service.annotations_next_v2(90,event)==1&&event.target==1&&event.context==map.context&&event.world.id==2&&event.world.world_position[0]==20&&event.attachment.native_slot==0,"world event preserves world coordinate units and empty attachment");
    require(edit(90,4,2)==202&&service.annotations_next_v2(90,event)==1&&event.target==2&&event.attachment.native_slot==2&&event.attachment.map_pixels[0]==200&&event.world.id==0,"native edit cannot alias world ID2");
    require(edit(90,3,2)==202&&service.annotations_next_v2(90,event)==1&&event.target==2&&event.attachment.native_slot==2,"native deletion selects attachment namespace");
    require(edit(1,4,2)==409,"waiting owner cannot forge native attachment UI action");
    require(edit(1,2,1)==202&&service.annotations_next_v2(1,event)==1,"world annotations remain editable while native ownership waits");
    const auto poll=std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++seq);
    require(service.exchange(poll,body)==200&&body.find("\"version\":3")!=body.npos,"typed UI protocol advertises descriptor version");
    auto bad=f;bad.attachments[0].reserved=1;require(service.annotations_publish_v3(90,bad)==-3,"native reserved field rejected");
    bad=f;bad.items[0].flags=2;require(service.annotations_publish_v3(90,bad)==-3,"world descriptor cannot masquerade as attachment");
    bad=f;bad.attachment_count=2;bad.attachments[1]=bad.attachments[0];require(service.annotations_publish_v3(90,bad)==-3,"duplicate native slot rejected");
    bad=f;bad.count=129;require(service.annotations_publish_v3(90,bad)==-3,"typed count bound before access");
    bad=f;bad.count=128;bad.attachment_count=1;require(service.annotations_publish_v3(90,bad)==-3,"shared typed capacity bounded");
    bad=f;bad.reserved=1;require(service.annotations_publish_v3(90,bad)==-3,"typed frame reserved field rejected");
    service.enable_map_editor_input(true);
    require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++seq)+"?status=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0&editor=90,1,"+std::to_string(map.context),body)==200&&service.map_editor_input_active(),"retained owner may hold active editor focus");
    now+=100;require(service.annotations_publish_v3(1,f)==1&&service.map_editor_input_active(),"later renewal cannot steal or clear active editor");
    service.annotations_hide(90);require(!service.map_editor_input_active(),"hidden owner releases editor input");
    require(service.annotations_status(1,status)==1&&status.granted==6,"oldest remaining arrival takes over");
    service.detach(1);require(service.annotations_status(50,status)==1&&status.granted==6,"unload hands off retained ownership");
    now+=100;f.revision=2;f.flags=2;require(service.annotations_publish_v3(50,f)==1,"owner may relinquish placement only");
    f.flags=7;require(service.annotations_publish_v3(90,f)==1,"released owner rejoins behind retained owner");
    require(service.annotations_status(50,status)==1&&status.granted==2,"native owner retained independently");
    require(service.annotations_status(90,status)==1&&status.granted==4,"placement independently granted after optout");
    now+=601;service.update_map(d,{0,0,400,300});require(service.annotations_status(90,status)==0&&status.version==0,"expired interaction status clears output");
    require(service.annotations_publish_v3(90,f)==1&&service.annotations_status(90,status)==1&&status.granted==6,"fresh arrival reacquires expired interactions");
    service.open_page();require(service.annotations_status(90,status)==0&&status.version==0,"page replacement releases retained ownership");
}
void annotation_tests() {
    uint64_t now=100,sequence=0;crml::ModDrawing service([&]{return now;});
    service.attach(1,"markers");service.attach(2,"other");service.enable_renderer(true);auto page=service.open_page();
    crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};
    service.update_map(d,{10,20,400,300});crml_map_state state{};service.map_read(1,state);
    crml_map_annotations f{};f.version=1;f.size=sizeof(f);f.context=state.context;f.revision=1;f.flags=1;f.up[2]=1;
    f.count=1;f.items[0].id=7;f.items[0].position[0]=20;f.items[0].position[1]=40;f.items[0].flags=1;f.items[0].rgba=0xffaaffff;
    strcpy_s(f.items[0].symbol,"A");strcpy_s(f.items[0].name,"<Marker>");strcpy_s(f.items[0].description,"Note");
    require(service.annotations_publish(1,f)==1,"annotation publish");
    const auto edit=[&](unsigned action,uint64_t revision=1,uint64_t id=7,uint64_t owner=1){std::string out;
        return service.exchange(std::string(crml::drawing_prefix)+"edit/"+std::to_string(page)+"/"+std::to_string(++sequence)+"/"+std::to_string(owner)+"/"+std::to_string(revision)+"/"+std::to_string(state.context)+"/"+std::to_string(action)+"/"+std::to_string(id)+"/4294967295/50/0.5/0.5/t41/t4e6577/t44657363",out);};
    require(edit(2)==202,"annotation update queued");crml_map_annotation_event event{};
    require(service.annotations_next(2,event)==0&&event.version==0,"annotation event owner isolation");
    require(service.annotations_next(1,event)==1&&event.item.position[0]==20&&event.item.position[1]==40&&event.item.distance==50,"update cannot move host-owned copied point");
    require(edit(2,2)==409&&edit(2,1,99)==404&&edit(2,1,7,2)==409,"stale revision, wrong row and other owner rejected");
    require(edit(1,1,0)==202&&service.annotations_next(1,event)==1&&std::abs(event.item.position[0]-50)<.001f&&std::abs(event.item.position[1]-100)<.001f,"create uses inverse projection on declared plane");
    for(unsigned i=0;i<16;++i)require(edit(2)==202,"bounded annotation queue admits capacity");
    require(edit(2)==429,"annotation queue refuses overflow");service.annotations_hide(1);require(service.annotations_next(1,event)==0,"hide drops pending edits");
    now+=100;require(service.annotations_publish(1,f)==1,"republish annotations");
    auto bad=f;bad.count=25;require(service.annotations_publish(1,bad)==-3,"oversized annotation count");
    bad=f;bad.items[0].symbol[0]='<';require(service.annotations_publish(1,bad)==-3,"unsupported symbol rejected");
    bad=f;bad.items[0].name[0]=char(0xff);require(service.annotations_publish(1,bad)==-3,"invalid UTF8 rejected");
    bad=f;bad.items[0].distance=std::numeric_limits<float>::quiet_NaN();require(service.annotations_publish(1,bad)==-3,"nonfinite visibility rejected");
    bad=f;bad.count=2;bad.items[1]=bad.items[0];require(service.annotations_publish(1,bad)==-3,"duplicate annotation identity rejected");
    now+=100;bad=f;strcpy_s(bad.items[0].name,"Changed");require(service.annotations_publish(1,bad)==-6,"same revision cannot replace item contents");
    require(edit(3)==202,"delete queued");d.offset[0]=1;service.update_map(d,{10,20,400,300});require(service.annotations_next(1,event)==0&&edit(2)==409,"projection change cancels stale interaction");
    service.map_read(1,state);f.context=state.context;require(service.annotations_publish(1,f)==1,"new context permits copied metadata");
    std::string out;require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++sequence),out)==200&&out.find("<Marker>")!=out.npos,"copied annotation serialized");
    require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(sequence),out)==409,"poll/edit shared replay sequence");
    require(edit(4,1,2)==409,"native attachment requires explicit owner opt-in");
    now+=100;f.flags=CRML_MAP_ANNOTATIONS_NATIVE_MARKERS;f.revision=2;
    require(service.annotations_publish(1,f)==1,"native metadata opt-in");
    const auto native_edit=[&](uint64_t id,float distance){return service.exchange(std::string(crml::drawing_prefix)+"edit/"+std::to_string(page)+"/"+std::to_string(++sequence)+"/1/2/"+std::to_string(state.context)+"/4/"+std::to_string(id)+"/4294967295/"+std::to_string(distance)+"/0.5/0.25/t32/t4e6577/t",out);};
    require(native_edit(0,0)==409&&native_edit(7,0)==409&&native_edit(2,50)==409,"native slot/range bounds");
    require(native_edit(2,0)==202&&service.annotations_next(1,event)==1&&event.action==4&&event.item.id==2&&event.item.flags==3&&event.item.position[0]==210&&event.item.position[1]==95&&event.item.position[2]==0,"native metadata carries exact map pixels, not invented world coordinates");
    f.items[0]=event.item;f.revision=3;now+=100;require(service.annotations_publish(1,f)==1,"publish copied native attachment");
    require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++sequence),out)==200&&out.find("\"native\":true")!=out.npos&&out.find("\"x\":0.5")!=out.npos,"native attachment round trip bypasses world projection");
    crml::sonar_projection::Snapshot sonar{};sonar.current.plane={0,0,0,1};sonar.current.reference={0,0,0,1};
    sonar.current.near_radius=20;sonar.current.far_radius=80;sonar.previous=sonar.current;sonar.interpolation=1;
    now+=501;service.update_sonar(sonar);
    require(service.annotations_publish(1,f)==1,"sonar can renew metadata while full map is closed");
    require(service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++sequence),out)==200&&out.find("\"annotations\":[]")!=out.npos&&out.find("\"native_metadata\":[{\"owner\":\"1\"")!=out.npos&&out.find("\"px\":210")!=out.npos,"sonar metadata uses native positions without inventing world coordinates");
    const auto current_native_edit=[&](){return service.exchange(std::string(crml::drawing_prefix)+"edit/"+std::to_string(page)+"/"+std::to_string(++sequence)+"/1/3/"+std::to_string(state.context)+"/4/2/4294967295/0/0.5/0.25/t32/t4e6577/t",out);};
    require(current_native_edit()==409,"closed full map cannot accept current-revision metadata edits");
    service.update_map(d,{10,20,400,300});crml_map_state reopened{};require(service.map_read(1,reopened)==1&&reopened.context==state.context,"map reopens with same metadata context");
    require(current_native_edit()==202&&service.annotations_next(1,event)==1,"same revision edit becomes valid only after map refresh");
    f.items[0].flags=CRML_MAP_ANNOTATION_NATIVE_MARKER;f.revision=4;now+=100;
    require(service.annotations_publish(1,f)==1,"publish read-only native metadata");
    const auto locked=std::string(crml::drawing_prefix)+"edit/"+std::to_string(page)+"/"+std::to_string(++sequence)+"/1/4/"+std::to_string(state.context)+"/4/2/4294967295/0/0.5/0.25/t32/t4e6577/t";
    require(service.exchange(locked,out)==409,"native attachment upsert respects read-only existing entry");
    bad=f;bad.items[0].position[2]=1;require(service.annotations_publish(1,bad)==-3,"native marker cannot carry invented height");
    now+=600;require(service.annotations_next(1,event)==0&&edit(2)==409,"expired annotation lease cannot accept edits");
    service.cancel(1);service.detach(1);
}
void hover_annotation_tests() {
    uint64_t now=100,seq=0;crml::ModDrawing service([&]{return now;});
    require(service.attach(20,"hover-owner")&&service.attach(1,"waiting-owner"),"hover owners attached");
    service.enable_renderer(true);const auto page=service.open_page();
    crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};
    service.update_map(d,{0,0,400,300});crml_map_state map{};require(service.map_read(20,map)==1,"hover map ready");
    crml_map_annotations_v3 f{};f.version=3;f.size=sizeof(f);f.context=map.context;f.revision=1;f.flags=CRML_MAP_ANNOTATIONS_PLACE_ACTION;f.up[2]=1;
    f.count=1;f.items[0].id=2;f.items[0].world_position[0]=20;f.items[0].world_position[1]=40;
    f.items[0].flags=CRML_MAP_ANNOTATION_EDITABLE;f.items[0].rgba=0xffffffff;
    strcpy_s(f.items[0].symbol,"W");strcpy_s(f.items[0].name,"World two");
    f.attachment_count=1;f.attachments[0].native_slot=2;f.attachments[0].flags=CRML_MAP_ANNOTATION_EDITABLE;
    f.attachments[0].rgba=0xffffffff;f.attachments[0].map_pixels[0]=200;f.attachments[0].map_pixels[1]=150;
    strcpy_s(f.attachments[0].symbol,"N");strcpy_s(f.attachments[0].name,"Native two");
    require(service.annotations_publish_v3(20,f)==1&&service.annotations_publish_v3(1,f)==1,"world ID2 and native slot2 published by competing owners");
    const std::string status="?status=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
    std::string body;
    const auto poll=[&](const std::string& suffix){return service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++seq)+status+suffix,body);};
    const auto selection=[&](uint64_t owner,uint64_t revision,uint64_t context,uint64_t id){return std::string("&hover=")+std::to_string(owner)+","+std::to_string(revision)+","+std::to_string(context)+","+std::to_string(id)+"&hover_box=0.1,0.1,0.4,0.8";};
    const auto action=[&](){return service.hovered_annotation_action({.2f,.4f});};
    const auto chosen=selection(20,1,map.context,2);
    require(!action(),"no UI selection leaves X to stock handler");
    require(poll(selection(1,1,map.context,2))==200&&!action(),"waiting owner cannot route X to its own annotation");
    for(unsigned i=0;i<16;++i){
        const auto edit=std::string(crml::drawing_prefix)+"edit2/"+std::to_string(page)+"/"+std::to_string(++seq)+
            "/20/1/"+std::to_string(map.context)+"/2/2/4294967295/0/0.2/0.4/t41/t456469746564/t/1";
        require(service.exchange(edit,body)==202,"fill owner-local edit queue");
    }
    require(poll(chosen)==200&&action(),"full guest queue consumes X without forwarding it to stock removal");
    crml_map_annotation_event_v2 event{};
    for(unsigned i=0;i<16;++i)require(service.annotations_next_v2(20,event)==1&&event.action==CRML_MAP_ANNOTATION_UPDATE,"queue remains unchanged by full-queue hover action");
    require(service.annotations_next_v2(20,event)==0,"full-queue hover did not enqueue a delete");
    require(poll(chosen)==200&&!service.hovered_annotation_action({.5f,.4f})&&
        !service.hovered_annotation_action({.2f,.9f})&&action()&&action(),
        "only the current cursor inside the selected hit rectangle consumes X");
    require(service.hovered_annotation_action({.1f,.1f})&&service.hovered_annotation_action({.4f,.8f})&&
        !service.hovered_annotation_action({.4001f,.8f}),"hit rectangle includes its edges and excludes moved pointer");
    require(service.annotations_next_v2(1,event)==0&&service.annotations_next_v2(20,event)==1&&
        event.action==CRML_MAP_ANNOTATION_DELETE&&event.target==CRML_MAP_ANNOTATION_TARGET_WORLD&&event.world.id==2&&event.attachment.native_slot==0,
        "hover queues exactly the owning world ID, never native slot2 or peer data");
    require(service.annotations_next_v2(20,event)==0&&action()&&service.annotations_next_v2(20,event)==0,
        "hover delete stays coalesced after guest consumes event until a new revision");
    require(poll("")==200&&!action(),"pointer absence revokes hover lease");
    require(poll(chosen)==200&&poll("&hover=20,1,"+std::to_string(map.context)+",2,3&hover_box=0.1,0.1,0.4,0.8")==400&&
        !action(),"malformed hover query revokes selection");
    for(const auto& invalid:{std::string("&hover=20,1,")+std::to_string(map.context)+",2",
                             std::string("&hover_box=0.1,0.1,0.4,0.8"),
                             std::string("&hover=20,1,")+std::to_string(map.context)+",2&hover_box=0.4,0.1,0.1,0.8",
                             std::string("&hover=20,1,")+std::to_string(map.context)+",2&hover_box=-0.1,0.1,0.4,0.8",
                             std::string("&hover=20,1,")+std::to_string(map.context)+",2&hover_box=nan,0.1,0.4,0.8"})
        require(poll(chosen)==200&&poll(invalid)==400&&!action(),"missing, inverted or nonfinite hover bounds revoke lease");
    for(const auto& wrong:{selection(20,2,map.context,2),selection(20,1,map.context+1,2),selection(20,1,map.context,99)})
        require(poll(wrong)==200&&!action(),"stale revision, context or ID cannot select a marker");
    require(poll(chosen)==200,"hover renewed");now+=199;require(action(),"hover remains valid before 200ms");
    ++now;require(!action(),"hover expires at 200ms without a renderer poll");
    now+=100;f.revision=2;f.flags=0;require(service.annotations_publish_v3(20,f)==1,"owner can release placement opt-in");
    require(poll(selection(20,2,map.context,2))==200&&!action(),"world hover needs PLACE_ACTION permission");
    service.annotations_hide(1);
    now+=100;service.update_map(d,{0,0,400,300});f.revision=3;f.flags=CRML_MAP_ANNOTATIONS_PLACE_ACTION;f.count=0;
    require(service.annotations_publish_v3(20,f)==1,"native-only attachment published");
    auto waiting=f;waiting.revision=2;waiting.count=1;
    require(service.annotations_publish_v3(1,waiting)==1,"waiting owner rejoins behind hover owner");
    require(poll(selection(20,3,map.context,2))==200&&!action(),"stock attachment never qualifies as world hover");
    now+=100;f.revision=4;f.count=1;f.items[0].flags=0;
    require(service.annotations_publish_v3(20,f)==1&&poll(selection(20,4,map.context,2))==200&&!action(),"read-only world item cannot be deleted");
    now+=100;service.update_map(d,{0,0,400,300});f.revision=5;f.items[0].flags=CRML_MAP_ANNOTATION_EDITABLE;f.items[0].distance=1;
    require(service.annotations_publish_v3(20,f)==1&&poll(selection(20,5,map.context,2))==200&&!action(),"distance-hidden world item cannot be deleted");
    now+=100;f.revision=6;f.items[0].distance=0;f.items[0].world_position[0]=200;
    require(service.annotations_publish_v3(20,f)==1&&poll(selection(20,6,map.context,2))==200&&!action(),"off-map world item cannot be deleted");
    now+=100;service.update_map(d,{0,0,400,300});f.revision=7;f.items[0].world_position[0]=20;
    require(service.annotations_publish_v3(20,f)==1,"visible world item republished");
    service.enable_map_editor_input(true);
    const auto editor="&editor=20,7,"+std::to_string(map.context);
    require(poll(selection(20,7,map.context,2)+editor)==200&&service.map_editor_input_active()&&
        !action(),"editor suffix is parsed after hover and editor focus suppresses X routing");
    require(poll(selection(20,7,map.context,2))==200&&!service.map_editor_input_active()&&action(),"closing editor restores exact hover routing");
    require(service.annotations_next_v2(20,event)==1&&event.target==CRML_MAP_ANNOTATION_TARGET_WORLD,"new revision permits one fresh delete");
    service.cancel(20);require(!action(),"owner cancel revokes hover");
    require(poll(selection(1,2,map.context,2))==200&&action(),"waiting placement owner takes over after cancel");
    service.open_page();require(!action(),"page replacement revokes hover");
}
void editor_focus_tests() {
    uint64_t now=100,seq=0;crml::ModDrawing service([&]{return now;});
    service.attach(1,"markers");service.enable_renderer(true);auto page=service.open_page();
    crml::map_projection::District d{};d.rotation={0,0,0,1};d.maximum={100,100,100};d.scale={1,1};
    service.update_map(d,{10,20,400,300});crml_map_state state{};service.map_read(1,state);
    crml_map_annotations f{};f.version=1;f.size=sizeof(f);f.context=state.context;f.revision=1;f.flags=2;f.up[2]=1;
    require(service.annotations_publish(1,f)==1,"focus owner publishes live map annotations");
    std::string body;
    auto focus=[&](const std::string& fields){return service.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++seq)+
        "?status=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"+(fields.empty()?"":"&editor="+fields),body);};
    const auto identity="1,1,"+std::to_string(state.context);
    require(focus(identity)==200&&!service.map_editor_input_active(),"unknown native input adapter cannot grant focus");
    service.enable_map_editor_input(true);
    require(focus(identity)==200&&service.map_editor_input_active(),"authenticated owner focus granted");
    require(focus(identity+",0")==400,"malformed focus rejected");
    now+=350;require(!service.map_editor_input_active(),"stalled renderer releases map input within 350ms");
    require(focus(identity)==200&&service.map_editor_input_active(),"live focus renewed");
    require(focus("")==200&&!service.map_editor_input_active(),"closed editor releases focus");
    for(const auto& wrong:{std::string("2,1,")+std::to_string(state.context),std::string("1,2,")+std::to_string(state.context),std::string("1,1,99")})
        require(focus(wrong)==200&&!service.map_editor_input_active(),"owner revision context focus isolation");
    require(focus(identity)==200&&service.map_editor_input_active(),"focus reacquired");
    service.annotations_hide(1);require(!service.map_editor_input_active(),"owner hide revokes immediately");
    now+=100;service.update_map(d,{10,20,400,300});require(service.annotations_publish(1,f)==1,"owner republish");
    require(focus(identity)==200&&service.map_editor_input_active(),"fresh owner can edit");
    service.open_page();require(!service.map_editor_input_active(),"page replacement revokes focus");
    require(focus(identity)==403&&!service.map_editor_input_active(),"old page cannot reacquire focus");
}
int main() {
    try {
        expanded_annotation_tests();map_tests();sonar_tests();sonar_reprojection_tests();annotation_v3_tests();annotation_tests();hover_annotation_tests();editor_focus_tests();
        {
            uint64_t stamp=100;crml::ModDrawing service([&]{return stamp;});service.enable_renderer(true);auto page=service.open_page();
            const auto base=std::string(crml::drawing_prefix)+std::to_string(page)+"/";
            std::string body,values="4,1,2,1,6,6,4,0,2,1,1,1,0,0,0,6,6,0,0,63,0,0,0,10";
            require(service.exchange(base+"1?status="+values,body)==200,"bounded renderer status piggybacks normal poll");
            require(service.diagnostics().find("\"reports\":1")!=std::string::npos&&service.diagnostics().find("\"fresh\":true")!=std::string::npos,"renderer report stored");
            require(service.exchange(base+"1?status="+values,body)==409,"renderer status obeys replay gate");
            for(const auto& bad:{std::string("0"),values+",0",std::string("1000001")+values.substr(1),std::string("-1")+values.substr(1)})
                require(service.exchange(base+"2?status="+bad,body)==400,"renderer status rejects malformed size/range");
            require(service.exchange(base+"2?unexpected="+values,body)==400,"unknown query rejected");
            stamp+=1500;require(service.diagnostics().find("\"fresh\":false")!=std::string::npos,"stale renderer status labelled");
            service.open_page();require(service.diagnostics().find("\"reports\":0")!=std::string::npos,"page reload resets renderer status");
        }
        uint64_t now{};crml::ModDrawing drawing([&]{return now;});auto f=frame();
        for(uint64_t i=1;i<=32;++i)require(drawing.attach(i,"mod-"+std::to_string(i)),"attach owner");
        require(!drawing.attach(33,"full")&&!drawing.attach(1,"duplicate"),"bounded owner registry");
        require(!drawing.available()&&drawing.publish(1,f)==-1,"renderer unavailable");
        drawing.enable_renderer(true);const auto page=drawing.open_page();require(page>0&&drawing.available(),"renderer page ready");
        require(drawing.publish(1,f)==1,"first frame admitted");
        f.segments[0].x1=.2f;strcpy_s(f.labels[0].text,"Guest mutation");
        std::vector<crml::ModDrawing::Surface> surfaces;
        require(drawing.poll(page,1,surfaces)==200&&surfaces.size()==1&&surfaces[0].frame.segments[0].x1==.1f&&std::string(surfaces[0].frame.labels[0].text).starts_with("Marker"),"guest frame copied");
        const auto first_revision=surfaces[0].revision;
        require(drawing.publish(1,f)==-4,"publish rate limited");
        now=100;
        auto bad=f;bad.segments[1].x2=std::numeric_limits<float>::quiet_NaN();
        require(drawing.publish(1,bad)==-3,"nonfinite geometry rejected");
        bad=f;bad.labels[0].text[0]=char(-1);require(drawing.publish(1,bad)==-3,"malformed UTF8 rejected");
        for(const auto* text:{"a\nb","a\x7f" "b","a\xc2\x85" "b","a\xe2\x80\xa8" "b","a\xe2\x80\xa9" "b"}){
            bad=f;strcpy_s(bad.labels[0].text,text);require(drawing.publish(1,bad)==-3,"control characters rejected");}
        bad=f;std::memset(bad.labels[0].text,'x',sizeof(bad.labels[0].text));require(drawing.publish(1,bad)==-3,"unterminated labels rejected");
        bad=f;bad.segment_count=129;require(drawing.publish(1,bad)==-3,"segment capacity checked");
        bad=f;bad.label_count=33;require(drawing.publish(1,bad)==-3,"label capacity checked");
        bad=f;bad.lifetime_ms=1001;require(drawing.publish(1,bad)==-3,"lease bound checked");
        bad=f;bad.lifetime_ms=99;require(drawing.publish(1,bad)==-3,"minimum lease checked");
        bad=f;bad.width=1;bad.x=.1f;require(drawing.publish(1,bad)==-3,"surface bounds checked");
        bad=f;bad.version=2;require(drawing.publish(1,bad)==-3,"version checked");
        bad=f;bad.size=0;require(drawing.publish(1,bad)==-3,"size checked");
        bad=f;bad.reserved[2]=1;require(drawing.publish(1,bad)==-3,"reserved checked");
        require(drawing.poll(page,2,surfaces)==200&&surfaces[0].revision==first_revision&&surfaces[0].frame.segments[0].x1==.1f,"invalid writes left old frame intact");
        require(drawing.publish(1,f)==1,"valid replacement after invalid writes");
        require(drawing.poll(page,3,surfaces)==200&&surfaces[0].revision>first_revision&&surfaces[0].frame.segments[0].x1==.2f,"atomic replacement revision");
        const auto replacement_revision=surfaces[0].revision;now=200;
        f.segments[127].x1=std::numeric_limits<float>::quiet_NaN();
        f.labels[31].text[0]=char(-1);f.labels[0].text[63]='x';
        require(drawing.publish(1,f)==1&&drawing.poll(page,4,surfaces)==200&&surfaces[0].revision==replacement_revision&&surfaces[0].remaining_ms==1000,"renewal keeps geometry revision and refreshes lease");
        for(uint64_t i=2;i<=4;++i)require(drawing.publish(i,f)==1,"four visible surfaces");
        require(drawing.publish(5,f)==-2,"fifth surface explicit contention");
        require(drawing.hide(1)==1&&drawing.hide(1)==0,"immediate idempotent hide");
        require(drawing.publish(1,f)==-4,"hide does not bypass publish rate");
        require(drawing.publish(5,f)==1,"hidden slot reusable by another owner");
        drawing.cancel(2);require(drawing.hide(2)==0,"release cleanup");
        drawing.detach(3);require(drawing.hide(3)==-1&&drawing.publish(3,f)==-1,"detach removes ownership");
        now=1199;require(drawing.poll(page,5,surfaces)==200&&surfaces.size()==2&&surfaces[0].remaining_ms==1,"last lease millisecond");
        now=1200;require(drawing.poll(page,6,surfaces)==200&&surfaces.empty(),"guest stall expires drawings");
        // Maximum frame exercises copied primitive bounds and JSON byte size.
        f=frame();f.segment_count=128;f.label_count=32;
        for(auto& segment:f.segments)segment=f.segments[0];
        for(auto& label:f.labels){label=f.labels[0];std::memset(label.text,'"',63);label.text[63]=0;}
        for(uint64_t i:{1ull,2ull,4ull,5ull})require(drawing.publish(i,f)==1,"dense bounded frame");
        std::string body;auto prefix=std::string(crml::drawing_prefix)+std::to_string(page)+"/";
        require(drawing.exchange(prefix+"7",body)==200&&body.size()<262144&&body.find("\\\"")!=body.npos,"bounded escaped wire snapshot");
        require(drawing.exchange(prefix+"7",body)==409&&body.empty(),"sequence replay rejected");
        for(const auto* suffix:{"0","-1","01","7/x","7/","18446744073709551616"})require(drawing.exchange(prefix+suffix,body)==400,"bad sequence rejected");
        const auto replacement=drawing.open_page();
        require(drawing.poll(page,7,surfaces)==403,"old page rejected");
        require(drawing.poll(replacement,1,surfaces)==200&&surfaces.empty(),"page replacement clears old frames");
        now+=100;require(drawing.publish(1,f)==1,"new page frame");drawing.enable_renderer(false);
        require(drawing.poll(replacement,2,surfaces)==503&&!drawing.available(),"renderer disable gate");
        drawing.enable_renderer(true);const auto current=drawing.open_page();
        require(drawing.poll(current,1,surfaces)==200&&surfaces.empty(),"renderer restart never revives frames");
        now=UINT64_MAX-99;require(drawing.publish(1,f)==-5,"deadline overflow rejected");
        std::cout<<"Drawing copied atomic frames, primitive/UTF8 bounds, rate/capacity, leases, owner cleanup and renderer protocol passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
