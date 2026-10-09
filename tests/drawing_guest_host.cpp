#include "runtime.h"
#include "mod_drawing.h"
#include "mod_storage.h"
#include "mod_feedback.h"
#include <thread>
#include <chrono>
#include "mod_settings.h"
#include "mod_lists.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
struct Navigation : crml::Gameplay {
    crml_navigation_state state{1,0,1,1,CRML_NAV_UP_VALID,0,{0,0,0},{0,1,0}};
    int result{1};uint64_t continuity{1};
    uint32_t capabilities() const noexcept override{return CRML_CAP_NAVIGATION_READ;}
    int noclip_poll(uint64_t,float) noexcept override{return -1;}
    void release(uint64_t) noexcept override{}
    int navigation_read(crml_navigation_state& out) noexcept override{out=state;return result;}
    int navigation_read_v2(crml_navigation_state_v2& out) noexcept override{
        out={};if(result!=1)return result;
        std::memcpy(&out,&state,sizeof(state));out.version=2;out.continuity=continuity;return 1;
    }
};
void route_test(const wchar_t* mods) {
    uint64_t now{},sequence{};Navigation nav;crml::ModSettings settings;
    crml::ModLists lists([&]{return now;});lists.enable_renderer(true);
    crml::ModDrawing drawing([&]{return now;});drawing.enable_renderer(true);auto page=drawing.open_page();
    crml::ModStorage storage(std::filesystem::path(mods).parent_path()/"marker-storage");
    crml::ModFeedback feedback([&]{return now;});feedback.enable_renderer(true);const auto feedback_page=feedback.open_page();
    std::unique_ptr<crml::Runtime> runtime_owner(new crml::Runtime([](const std::string& text){std::cout<<text<<'\n';},&nav,nullptr,{},&storage,&settings,&feedback,[&]{return now;},nullptr,&drawing,&lists));
    auto* runtime=runtime_owner.get();
    runtime->load(mods);require(runtime->active()==1&&!runtime->failures(),"compiled route guest loaded");
    const auto setting=[&](const std::string& key){for(const auto& group:settings.snapshot())for(const auto& item:group.items)if(key==item.definition.key)return std::pair{group.owner,item.state};throw std::runtime_error("missing route setting "+key);};
    const auto set=[&](const std::string& key,double value){const auto [owner,item]=setting(key);require(settings.set(owner,item.handle,value,item.revision)>=0,"route setting mutation");};
    const auto name=[&](const std::string& value){const auto [owner,item]=setting("marker_name");require(settings.set_text(owner,item.handle,value,item.revision)>=0,"marker name mutation");};
    const auto tick=[&](uint64_t advance,float x){now+=advance;nav.state.position[0]=x;++nav.state.sequence;runtime->tick(static_cast<float>(advance)/1000.f);require(!runtime->failures(),"route guest trapped");};
    const auto snapshot=[&](){std::vector<crml::ModDrawing::Surface> out;require(drawing.poll(page,++sequence,out)==200,"route drawing snapshot");return out;};
    const auto frame=[&](){auto out=snapshot();require(out.size()==1,"route frame missing");return out[0].frame;};
    tick(0,0);require(frame().segment_count==0,"initial route seeds only one point");
    tick(600,2);require(frame().segment_count==0,"gap after clock-zero sample must not invent a connector");
    tick(200,4);auto f=frame();require(f.segment_count==1&&std::abs(f.segments[0].y1-.525f)<.0001f&&f.segments[0].y2==.5f,"guest projects real copied navigation onto movement plane");
    name("Caf\xc3\xa9 \xf0\x9f\x98\x80");set("mark",1);tick(200,6);f=frame();
    require(f.label_count==4&&std::string(f.labels[2].text)=="Caf\xc3\xa9 \xf0\x9f\x98\x80 [+0u]"&&setting("mark").second.value==0,"Unicode marker and height copied and action reset");
    name("");set("mark",1);tick(200,7);require(frame().label_count==4&&setting("mark").second.value==0,"empty marker refused/reset");
    set("clear",1);tick(200,8);f=frame();require(!f.segment_count&&f.label_count==3&&setting("clear").second.value==0,"clear resets trail, markers and switch");
    name("M");for(unsigned i=0;i<129;++i){set("mark",1);tick(200,8);}
    require(frame().label_count==32&&setting("mark").second.value==0,"128-marker collection respects the smaller schematic label budget");
    set("visible",0);tick(1,8);require(snapshot().empty(),"hide bypasses next-frame throttle");
    set("visible",1);tick(200,8);require(frame().label_count==3&&!frame().segment_count,"hide clears session data");
    tick(200,9);require(frame().segment_count==1,"route resumed");
    ++nav.state.generation;++nav.continuity;tick(200,10);require(frame().segment_count==0,"observed player continuity change clears");
    tick(200,11);nav.state.flags|=CRML_NAV_TELEPORTED;tick(200,12);require(!frame().segment_count,"teleport clears");
    nav.state.flags=CRML_NAV_UP_VALID;tick(200,13);tick(200,40);require(!frame().segment_count,"large sample jump clears");
    nav.result=-2;tick(200,40);require(snapshot().empty(),"stale navigation clears drawing");
    nav.result=1;tick(200,40);require(!frame().segment_count,"valid sample after gap restarts trail");
    nav.state.flags=CRML_NAV_UP_VALID|CRML_NAV_CONTROLLER_DISABLED;tick(200,40);require(snapshot().empty(),"disabled controller hides");
    nav.state.flags=CRML_NAV_UP_VALID|CRML_NAV_KEYFRAMED;tick(200,40);require(snapshot().empty(),"keyframed controller hides");
    nav.state.flags=0;tick(200,40);require(snapshot().empty(),"unknown up hides schematic");
    nav.state.flags=CRML_NAV_UP_VALID;nav.state.up[0]=1;nav.state.up[1]=0;tick(200,40);require(frame().label_count==3,"parallel basis fallback stays finite");
    nav.state.up[0]=0;nav.state.up[1]=1;set("clear",1);
    for(unsigned i=0;i<140;++i)tick(200,float(i%2));
    require(frame().segment_count<=128&&frame().segment_count>0,"bounded overview geometry after compaction");
    drawing.enable_renderer(false);for(unsigned i=0;i<8;++i)tick(200,float(i%2));
    require(runtime->active()==1&&!runtime->failures(),"renderer unavailable does not trap example");
    drawing.enable_renderer(true);page=drawing.open_page();sequence=0;tick(200,0);require(!snapshot().empty(),"renderer recovers with fresh lease");
    crml::map_projection::District district{};district.rotation={0,0,0,1};
    district.maximum={100,100,100};district.scale={1,1};
    drawing.update_map(district,{0,0,400,300});tick(200,1);
    auto mapped=snapshot();require(mapped.size()==2,"guest publishes schematic and native map together");
    require(std::any_of(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_FULL&&s.frame.segment_count>0;}),"guest world route reaches native map layer");
    nav.result=0;drawing.update_map(district,{0,0,400,300});tick(200,1);
    mapped=snapshot();require(mapped.size()==1&&mapped[0].target==CRML_MAP_FULL,"brief unavailable navigation hides schematic but retains recent map route");
    for(unsigned i=0;i<60;++i){drawing.update_map(district,{0,0,400,300});tick(200,1);}
    mapped=snapshot();require(mapped.size()==1&&mapped[0].target==CRML_MAP_FULL,"continuous native player-map stream retains route across a twelve second pause");
    nav.result=1;++nav.state.generation;drawing.update_map(district,{0,0,800,300});tick(200,2);
    mapped=snapshot();require(mapped.size()==2,"sample-gap generation and map resize do not erase continuous map route");
    nav.result=0;for(unsigned i=0;i<4;++i){drawing.update_map(district,{0,0,800,300});tick(200,2);}
    nav.result=1;++nav.continuity;++nav.state.generation;drawing.update_map(district,{0,0,800,300});tick(200,2);
    mapped=snapshot();require(mapped.size()==1&&mapped[0].target==0&&!mapped[0].frame.segment_count,"observed player replacement clears even during continuous map pause");
    drawing.update_map(district,{0,0,800,300});tick(200,3);
    nav.result=0;tick(800,2);require(snapshot().empty(),"lost native projection expires map layer");
    tick(400,2);drawing.update_map(district,{0,0,800,300});tick(200,2);
    require(snapshot().empty(),"new native context cannot revive route during absent navigation");
    nav.result=1;drawing.update_map(district,{0,0,800,300});tick(200,2);
    mapped=snapshot();require(mapped.size()==2&&mapped[0].frame.segment_count==1,"same lifetime after an expired map stream retains old route without a gap connector");
    drawing.update_map(district,{0,0,400,300});tick(200,3);
    require(snapshot().size()==2,"native map resumes with a fresh route");
    for(unsigned i=0;i<5;++i)tick(200,float(4+i));
    drawing.update_map(district,{0,0,400,300});tick(200,9);
    mapped=snapshot();require(mapped.size()==2&&mapped[1].frame.segment_count>0,"closing and reopening map retains route while player samples remain continuous");
    // Straight paths should not spend the entire route budget on redundant points.
    set("clear",1);tick(200,0);
    for(unsigned i=1;i<40;++i){drawing.update_map(district,{0,0,400,300});tick(200,float(i));}
    mapped=snapshot();const auto path=std::find_if(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_FULL;});
    require(path!=mapped.end()&&path->frame.segment_count==1&&path->frame.segments[0].x1==0,"straight route coalesces while retaining its start");
    require(path->frame.segments[0].rgba==0x9adaeaff&&path->frame.segments[0].width_vh==.20f,"recent route endpoint has readable style");
    nav.result=0;tick(800,39);nav.result=1;nav.state.age_ms=75;
    drawing.update_map(district,{0,0,400,300});tick(0,39);
    mapped=snapshot();require(mapped.empty(),"reacquisition waits for next guest publication tick");
    drawing.update_map(district,{0,0,400,300});tick(200,39);
    drawing.update_map(district,{0,0,400,300});tick(200,40);
    drawing.update_map(district,{0,0,400,300});tick(200,41);
    require(snapshot().size()==2,"nonzero-age fresh samples eventually adopt the reacquired map");
    // Long, turning route must retain its actual beginning, not just the latest
    // 121 points. Every tick runs inside the unchanged real Wasm fuel allowance.
    nav.state.age_ms=0;set("clear",1);tick(200,0);
    auto large=district;large.maximum={5000,100,5000};
    for(unsigned i=1;i<=2400;++i){
        nav.state.position[2]=float(i%2);
        drawing.update_map(large,{0,0,400,300});tick(200,float(i));
    }
    mapped=snapshot();auto long_path=std::find_if(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_FULL;});
    require(long_path!=mapped.end()&&long_path->frame.segment_count<=128&&long_path->frame.segment_count>24,"long route fits drawing budget without trapping");
    require(long_path->frame.segments[0].x1==0,"long turning route retains session start after thousands of samples");
    const auto old_segments=long_path->frame.segment_count;
    name("Before pause");set("mark",1);drawing.update_map(large,{0,0,400,300});tick(200,2400);
    nav.result=0;tick(5000,2400);require(snapshot().empty(),"unobserved map/player gap hides expired drawing");
    drawing.update_map(large,{0,0,400,300});tick(200,2400);require(snapshot().empty(),"changed map context cannot publish stale coordinates");
    nav.result=1;++nav.state.generation;drawing.update_map(large,{0,0,400,300});tick(200,2450);
    mapped=snapshot();long_path=std::find_if(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_FULL;});
    require(long_path!=mapped.end()&&long_path->frame.segments[0].x1==0&&long_path->frame.label_count==0,"same continuity preserves old path and marker after five second gap and movement");
    require(long_path->frame.segment_count<=old_segments,"unobserved movement does not create a straight shortcut");
    for(unsigned i=0;i<long_path->frame.segment_count;++i)
        require(long_path->frame.segments[i].x2<.485f,"no connector from old endpoint to new post-gap position");
    ++nav.continuity;drawing.update_map(large,{0,0,400,300});tick(200,2451);
    mapped=snapshot();require(mapped.size()==1&&mapped[0].frame.segment_count==0&&mapped[0].frame.label_count==3,"actual lifetime change still clears old route and marker");
    // Many small disconnected runs must remain bounded without connecting gaps.
    set("clear",1);tick(200,0);
    for(unsigned i=0;i<150;++i){
        nav.result=0;tick(200,0);nav.result=1;tick(200,0);tick(200,1);
    }
    f=frame();require(f.segment_count>0&&f.segment_count<=128,"many disconnected sections stay bounded");
    for(unsigned i=0;i<f.segment_count;++i)
        require(std::abs(f.segments[i].y2-f.segments[i].y1)<.013f,"section eviction does not invent connectors");
    set("clear",1);tick(200,0);
    for(unsigned i=1;i<=60;++i)tick(200,float(i));
    f=frame();require(f.segment_count==1&&f.segments[0].y1>.89f&&f.segments[0].y2==.5f,"fallback clips a long segment instead of dropping its visible portion");
    set("clear",1);nav.state.position[2]=0;tick(200,40);
    nav.state.age_ms=0;nav.state.position[1]=10;name("High");set("mark",1);
    drawing.update_map(district,{0,0,400,300});tick(200,41);
    nav.state.position[1]=0;drawing.update_map(district,{0,0,400,300});tick(200,41);
    mapped=snapshot();const auto elevated=std::find_if(mapped.begin(),mapped.end(),[](const auto& s){return s.target==0;});
    require(elevated!=mapped.end()&&elevated->frame.label_count==4&&std::string(elevated->frame.labels[2].text)=="High [+10u]","marker height measured along current local up");
    require((elevated->frame.segments[elevated->frame.segment_count-1].rgba&0xffffff00)==0x7ae8ac00,"above-player route has distinct height color");
    nav.state.position[1]=15;drawing.update_map(district,{0,0,400,300});tick(200,41);
    mapped=snapshot();const auto below=std::find_if(mapped.begin(),mapped.end(),[](const auto& s){return s.target==0;});
    require(below!=mapped.end()&&std::string(below->frame.labels[2].text)=="High [-5u]","marker height becomes negative below player");
    crml::sonar_projection::Snapshot sonar{};
    sonar.current.plane={0,0,0,1};sonar.current.reference={0,0,0,1};
    sonar.current.near_radius=20;sonar.current.far_radius=80;sonar.previous=sonar.current;sonar.interpolation=1;
    nav.state.position[1]=0;set("clear",1);drawing.update_sonar(sonar);tick(200,0);
    drawing.update_sonar(sonar);drawing.update_map(district,{0,0,400,300});tick(200,1);
    mapped=snapshot();
    require(std::any_of(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_SONAR&&s.frame.segment_count==1;})&&
            std::none_of(mapped.begin(),mapped.end(),[](const auto& s){return s.target==0;}),"guest uses native minimap and hides separate schematic");
    drawing.update_sonar(sonar);nav.state.flags|=CRML_NAV_CONTROLLER_DISABLED;tick(200,1);
    mapped=snapshot();require(std::none_of(mapped.begin(),mapped.end(),[](const auto& s){return s.target==CRML_MAP_SONAR;}),"disabled controller clears minimap route");
    nav.state.flags=CRML_NAV_UP_VALID;tick(600,2);
    mapped=snapshot();require(std::any_of(mapped.begin(),mapped.end(),[](const auto& s){return s.target==0;}),"expired minimap projection restores schematic fallback");
    // The full map editor submits bounded annotation events to this actual Wasm.
    nav.state.up[1]=0;nav.state.up[2]=1;set("clear",1);
    drawing.update_map(district,{0,0,400,300});tick(200,2);
    const auto hex=[](std::string_view value){std::string out="t";const char* digits="0123456789abcdef";for(unsigned char c:value){out+=digits[c>>4];out+=digits[c&15];}return out;};
    const auto packet=[&](){std::string out;require(drawing.exchange(std::string(crml::drawing_prefix)+std::to_string(page)+"/"+std::to_string(++sequence),out)==200,"annotation packet");return out;};
    const auto field=[](const std::string& json,const std::string& key){auto at=json.find("\"annotations\":[");require(at!=json.npos,"annotations missing");at=json.find("\""+key+"\":\"",at);require(at!=json.npos,"annotation field absent");at+=key.size()+4;return json.substr(at,json.find('"',at)-at);};
    const auto edit=[&](unsigned action,unsigned target,const std::string& id,const std::string& label,unsigned color,float distance){
        const auto data=packet();const auto url=std::string(crml::drawing_prefix)+"edit2/"+std::to_string(page)+"/"+std::to_string(++sequence)+"/"+field(data,"owner")+"/"+field(data,"revision")+"/"+field(data,"context")+"/"+std::to_string(action)+"/"+id+"/"+std::to_string(color)+"/"+std::to_string(distance)+"/0.02/1/t41/"+hex(label)+"/"+hex("A map description")+"/"+std::to_string(target);
        std::string out;require(drawing.exchange(url,out)==202,"map edit accepted");drawing.update_map(district,{0,0,400,300});tick(200,2);
    };
    edit(4,CRML_MAP_ANNOTATION_TARGET_NATIVE,"2","Map place",0x7ae8ffff,0);
    auto data=packet();require(data.find("Map place")!=data.npos&&data.find("A map description")!=data.npos,"Wasm applies map creation and description");
    auto id=field(data,"id");nav.result=0;
    edit(2,CRML_MAP_ANNOTATION_TARGET_NATIVE,id,"Renamed",0xeda9ffff,0);
    data=packet();require(data.find("Renamed")!=data.npos&&data.find("\"native\":true")!=data.npos,"editing works while map pauses player samples");
    nav.result=1;drawing.update_map(district,{0,0,400,300});tick(200,2);
    nav.state.flags=0;
    for(unsigned i=0;i<8;++i){drawing.update_sonar(sonar);tick(200,2);}
    data=packet();require(data.find("\"annotations\":[]")!=data.npos&&data.find("\"native_metadata\":[{\"owner\":")!=data.npos&&data.find("\"rgba\":3987341311")!=data.npos,"native metadata survives map close and unavailable height without inventing world coordinates");
    nav.result=0;drawing.update_map(district,{0,0,400,300});tick(200,2);
    data=packet();require(data.find("Renamed")!=data.npos,"reopened map retains native metadata without a new player sample");
    nav.result=1;nav.state.flags=CRML_NAV_UP_VALID;drawing.update_map(district,{0,0,400,300});tick(200,2);
    edit(3,CRML_MAP_ANNOTATION_TARGET_NATIVE,id,"Renamed",0xeda9ffff,0);require(packet().find("Renamed")==std::string::npos,"guest deletes selected marker");
    for(unsigned i=1;i<=6;++i)edit(4,CRML_MAP_ANNOTATION_TARGET_NATIVE,std::to_string(i),"Marker "+std::to_string(i),0xffe09aff,0);
    for(unsigned i=0;i<18;++i){set("mark",1);drawing.update_map(district,{0,0,400,300});tick(200,2);}
    data=packet();require(data.find("Marker 6")!=data.npos&&data.find("\"create\":true")!=data.npos,"24 map markers fit real guest fuel with room for more");
    const auto world_at=data.find("\"native\":false");require(world_at!=data.npos,"world marker published separately from native slots");
    const auto world_id_at=data.rfind("\"id\":\"",world_at);require(world_id_at!=data.npos,"world marker ID present");
    const auto world_id_begin=world_id_at+6;
    const auto world_id=data.substr(world_id_begin,data.find('"',world_id_begin)-world_id_begin);
    edit(2,CRML_MAP_ANNOTATION_TARGET_WORLD,world_id,"World edit",0x7ae8ffff,0);
    require(packet().find("World edit")!=data.npos,"typed world edit reaches route guest");
    edit(3,CRML_MAP_ANNOTATION_TARGET_WORLD,world_id,"World edit",0x7ae8ffff,0);
    require(packet().find("World edit")==data.npos,"typed world delete reaches route guest");
    name("Replacement");set("mark",1);drawing.update_map(district,{0,0,400,300});tick(200,2);
    for(unsigned i=0;i<200;++i){drawing.update_map(district,{0,0,400,300});drawing.update_sonar(sonar);tick(200,float(2+i%2));}
    mapped=snapshot();require(mapped.size()==2,"full route and 24 editable markers fit fuel/command budgets across both maps");
    require(std::any_of(mapped.begin(),mapped.end(),[](const auto& layer){return layer.frame.segment_count>100;}),"stress check includes compacted route geometry");
    set("clear",1);drawing.update_map(district,{0,0,400,300});tick(200,2);
    for(unsigned i=0;i<122;++i){
        drawing.update_map(district,{0,0,400,300});
        require(drawing.placement_action({10.f+30.f*(i%12),10.f+25.f*(i/12)},true),"native overflow action admitted");
        tick(200,2);
    }
    require(!drawing.placement_action({395,295},true),"guest collection caps at 128 without replacing older markers");
    data=packet();require(data.find("\"create\":false")!=data.npos,"creation disabled at expanded capacity");
    // Save metadata, settle the worker, then recreate the Wasm instance.
    for(unsigned i=0;i<200;++i){drawing.update_map(district,{0,0,400,300});drawing.update_sonar(sonar);tick(200,2);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    require(storage.status("route-sketch")==2,"marker archive reached durable commit");
    runtime->shutdown();runtime_owner.reset(new crml::Runtime([](const std::string& text){std::cout<<text<<'\n';},&nav,nullptr,{},&storage,&settings,&feedback,[&]{return now;},nullptr,&drawing,&lists));runtime=runtime_owner.get();runtime->load(mods);require(runtime->active()==1&&!runtime->failures(),"persisted route example reloads");
    for(unsigned i=0;i<50;++i){drawing.update_map(district,{0,0,800,600});tick(200,2);}
    data=packet();require(data.find("Marker 6")!=data.npos,"native metadata survives Wasm restart and map resizing");
    set("clear",1);drawing.update_map(district,{0,0,800,600});tick(200,2);
    require(packet().find("Marker 6")!=std::string::npos,"Clear route preserves saved native details as its help text promises");
    // Admission uses 800x600, but the helper shrinks to 400x300 before the
    // guest consumes the event. Refuse persistence instead of poisoning UVs.
    edit(2,CRML_MAP_ANNOTATION_TARGET_NATIVE,"6","Resize race",0xffe09aff,0);
    require(packet().find("Resize race")==std::string::npos,"resize race retains saved metadata");
    edit(3,CRML_MAP_ANNOTATION_TARGET_NATIVE,"6","Marker 6",0xffe09aff,0);
    bool saved_receipt=false;
    for(unsigned i=0;i<120;++i){drawing.update_map(district,{0,0,400,300});tick(200,2);std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<crml::ModFeedback::Message> receipts;require(feedback.poll(feedback_page,i+1,{},receipts)==200,"feedback poll");
        saved_receipt|=std::any_of(receipts.begin(),receipts.end(),[](const auto& m){return m.text=="Marker details saved.";});}
    require(storage.status("route-sketch")==2,"reset details reaches durable commit");
    // A fast commit may arrive within the feedback rate limit. The receipt
    // must eventually replace Saving instead of being silently dropped.
    require(saved_receipt,"durable receipt survives feedback throttling");
    std::vector<unsigned char> saved(65536);const int saved_size=storage.read("route-sketch",saved);
    require(saved_size>16,"saved archive readable");uint32_t saved_count{};std::memcpy(&saved_count,saved.data()+8,4);require(saved_count==127,"reset removes saved metadata instead of only hiding it");
    auto different=district;different.maximum[0]+=500;drawing.update_map(different,{0,0,800,600});tick(200,2);
    require(packet().find("Marker 6")==std::string::npos,"different map geometry cannot adopt saved metadata");
    for(unsigned i=0;i<20;++i){drawing.update_map(district,{0,0,400,300});tick(200,2);}
    require(packet().find("Marker 1")!=std::string::npos,"original map geometry restores saved details before bulk deletion");
    drawing.update_map(district,{0,0,400,300});tick(200,3);
    const auto before_delete=snapshot();const auto route_before=std::find_if(before_delete.begin(),before_delete.end(),[](const auto& s){return s.target==CRML_MAP_FULL;});
    require(route_before!=before_delete.end()&&route_before->frame.segment_count>0,"route exists before bulk marker deletion");
    const auto route_segments=route_before->frame.segment_count;
    set("delete_markers",1);drawing.update_map(district,{0,0,400,300});tick(200,3);
    require(setting("delete_markers").second.value==0&&packet().find("Marker 1")!=std::string::npos,"unconfirmed marker deletion is rejected and action resets");
    set("confirm_markers",1);set("delete_markers",1);drawing.update_map(district,{0,0,400,300});tick(200,3);
    require(setting("confirm_markers").second.value==0&&setting("delete_markers").second.value==0&&packet().find("Marker 1")==std::string::npos,"confirmed deletion clears guest marker details");
    const auto after_delete=snapshot();const auto route_after=std::find_if(after_delete.begin(),after_delete.end(),[](const auto& s){return s.target==CRML_MAP_FULL;});
    require(route_after!=after_delete.end()&&route_after->frame.segment_count==route_segments,"bulk marker deletion preserves the route");
    for(unsigned i=0;i<80;++i){drawing.update_map(district,{0,0,400,300});tick(200,3);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    require(storage.status("route-sketch")==2,"bulk marker deletion reaches durable commit");
    std::vector<unsigned char> cleared(65536);const int cleared_size=storage.read("route-sketch",cleared);uint32_t cleared_count=1;
    if(cleared_size>=16)std::memcpy(&cleared_count,cleared.data()+8,4);
    require(cleared_size==16&&cleared_count==0,"bulk marker deletion persists an empty archive");
    runtime->shutdown();runtime_owner.reset(new crml::Runtime([](const std::string& text){std::cout<<text<<'\n';},&nav,nullptr,{},&storage,&settings,&feedback,[&]{return now;},nullptr,&drawing,&lists));runtime=runtime_owner.get();runtime->load(mods);
    require(runtime->active()==1&&!runtime->failures(),"route guest reloads after bulk deletion");
    for(unsigned i=0;i<20;++i){drawing.update_map(district,{0,0,400,300});tick(200,3);}
    require(packet().find("Marker 1")==std::string::npos,"deleted marker details do not return after reload");
    runtime->shutdown();require(snapshot().empty()&&packet().find("\"annotations\":[]")!=std::string::npos&&!runtime->failures(),"shutdown removes annotations and route");
    std::cout<<"Compiled route sketch geometry, Unicode markers, limits, clear/hide, gaps/generation/teleports, invalid context and renderer lifecycle passed\n";
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3&&std::wstring_view(argv[2])==L"--route"){route_test(argv[1]);return 0;}
        require(argc==4,"Expected mods-A mods-B-or-dash renderer-enabled");
        uint64_t now{},sequence{};crml::ModDrawing drawing([&]{return now;});
        const bool enabled=std::wstring_view(argv[3])==L"1";
        drawing.enable_renderer(enabled);const auto page=drawing.open_page();
        crml::map_projection::District district{};district.rotation={0,0,0,1};district.maximum={100,100,100};district.scale={1,1};
        drawing.update_map(district,{0,0,400,300});
        auto log=[](const std::string& text){std::cout<<text<<'\n';};
        crml::Runtime a(log,nullptr,nullptr,{},nullptr,nullptr,nullptr,[&]{return now;},nullptr,&drawing);
        crml::Runtime b(log,nullptr,nullptr,{},nullptr,nullptr,nullptr,[&]{return now;},nullptr,&drawing);
        a.load(argv[1]);if(std::wstring_view(argv[2])!=L"-")b.load(argv[2]);
        for(std::string line;std::getline(std::cin,line);){
            std::istringstream in(line);std::string op;in>>op;
            if(op=="frames"||op=="label"){
                std::vector<crml::ModDrawing::Surface> surfaces;
                require(drawing.poll(page,++sequence,surfaces)==200,"snapshot unavailable");
                if(op=="frames"){size_t n{};in>>n;require(surfaces.size()==n,"frame count: "+line);}
                else {std::string text;in>>text;require(std::any_of(surfaces.begin(),surfaces.end(),[&](const auto& s){return s.frame.label_count&&s.frame.labels[0].text==text;}),"missing label: "+line);}
            }else if(op=="clock"){in>>now;}
            else if(op=="tick_a"||op=="tick_b"){unsigned ms{};in>>ms;now+=ms;(op=="tick_a"?a:b).tick(ms/1000.f);}
            else if(op=="shutdown_a"||op=="shutdown_b"){(op=="shutdown_a"?a:b).shutdown();}
            else if(op=="reload_a"){a.load(argv[1]);}
            else if(op=="active_a"||op=="active_b"){size_t n{};in>>n;require((op=="active_a"?a:b).active()==n,"active count: "+line);}
            else if(op=="failures_a"||op=="failures_b"){size_t n{};in>>n;require((op=="failures_a"?a:b).failures()==n,"failure count: "+line);}
            else throw std::runtime_error("Unknown command "+op);
            require(!in.fail(),"invalid command "+line);
        }
        std::cout<<"Drawing Wasm scenario passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
