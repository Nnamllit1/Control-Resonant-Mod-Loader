#include "mod_settings.h"
#include "settings_ui.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
crml_setting_definition definition() {
    crml_setting_definition d{};d.version=1;d.kind=CRML_SETTING_NUMBER;
    std::strcpy(d.key,"speed");std::strcpy(d.label,"Flight \"speed\"");std::strcpy(d.description,"Movement rate");
    d.initial=2;d.minimum=0;d.maximum=10;d.step=0.25;return d;
}
void text_settings() {
    crml::ModSettings settings;
    require(settings.attach(1,"names") && settings.attach(2,"other"),"text owners");
    crml_text_setting_definition d{1,4,"name","Marker name","A plain single-line value",""};
    require(settings.define_text(1,d)==1 && settings.define_text(2,d)==1,"text owner-local handles");
    require(settings.define_text(1,d)==-4,"shared duplicate text key");
    auto n=definition();std::strcpy(n.key,"name");require(settings.define(1,n)==-4,"numeric duplicate of text key");
    std::strcpy(n.key,"number");require(settings.define(1,n)==2,"shared numeric/text namespace");
    crml_text_setting_value text{};std::memset(&text,0x5a,sizeof(text));const auto sentinel=text;
    require(settings.read_text(1,2,text)==-3 && std::memcmp(&text,&sentinel,sizeof(text))==0,"wrong-kind text read preserves output");
    require(settings.set(1,1,1)==-3 && settings.set_text(1,2,"a")==-3,"cross-kind writes refused");
    require(settings.read_text(1,1,text)==1 && text.version==1 && text.handle==1 && text.length==0 && text.max_bytes==4 && text.revision==1,"empty initial text");
    const std::string emoji="\xf0\x9f\x8c\x8d";
    require(settings.set_text(1,1,emoji,1)==1 && settings.set_text(1,1,emoji,2)==0,"UTF8 exact byte boundary/no-op");
    require(settings.set_text(1,1,emoji+"x",2)==-3 && settings.set_text(1,1,"new",1)==-4,"byte limit and stale write");
    for(const auto invalid:{std::string("\xff"),std::string("\xc0\x80"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),
                           std::string("\xc2\x85"),std::string("\xe2\x80\xa8"),std::string("\xe2\x80\xa9"),std::string("a\0b",3),std::string("\t"),std::string("\n")})
        require(settings.set_text(1,1,invalid,2)==-3,"invalid UTF8/control refused");
    require(settings.read_text(1,1,text)==1 && text.revision==2 && text.length==4 && std::string(text.value)==emoji,"rejections preserve value and revision");
    require(settings.read_text(2,1,text)==1 && text.length==0 && text.revision==1,"foreign owner unchanged");
    require(settings.set_text(1,1,"",2)==1 && settings.read_text(1,1,text)==1 && text.value[0]==0 && text.revision==3,"clear text");
    std::array<crml_setting_value,32> values{};
    require(settings.read(1,values)==2 && values[0].kind==CRML_SETTING_TEXT && values[0].value==0 && values[0].revision==3,"legacy snapshot text metadata");
    for(int i=0;i<7;++i) {
        auto bad=d;std::strcpy(bad.key,"invalid");
        if(i==0)bad.version=2;if(i==1)bad.max_bytes=0;if(i==2)bad.max_bytes=256;
        if(i==3)std::memset(bad.initial,'x',sizeof(bad.initial));
        if(i==4)std::strcpy(bad.initial,"longer");if(i==5)std::strcpy(bad.initial,"\xc2\x85");
        if(i==6)std::strcpy(bad.key,"/path");require(settings.define_text(1,bad)==-3,"invalid text descriptor");
    }
    crml::SettingsUi ui(settings);ui.enable(true);
    const auto base=std::string(crml::settings_prefix)+std::to_string(ui.open_page())+"/";std::string body;
    require(ui.exchange(base+"1/1/1/3/tf09f8c8d",body)==200 && body.find("\"result\":1")!=body.npos,"renderer UTF8 decoded");
    require(settings.read_text(1,1,text)==1 && std::string(text.value)==emoji,"renderer shares text value");
    require(ui.exchange(base+"2/1/1/3/t61",body)==200 && body.find("\"result\":-4")!=body.npos,"renderer stale text rejected");
    for(const auto bad:{"t1","tzz","t61/extra"})require(ui.exchange(base+"3/1/1/4/"+bad,body)==400,"malformed hex rejected");
    require(ui.exchange(base+"3/1/1/4/t00",body)==200 && body.find("\"result\":-3")!=body.npos,"decoded NUL refused");
    require(ui.exchange(base+"4/1/1/4/t",body)==200 && body.find("\"result\":1")!=body.npos,"renderer empty text accepted");
    require(ui.exchange(base+"5/1/1/5/t3c223e",body)==200 && body.find("<\\\">")!=body.npos,"text JSON escaping");
    settings.detach(1);require(ui.exchange(base+"6/1/1/6/t61",body)==200 && body.find("\"result\":-1")!=body.npos,"late text edit after detach refused");
    require(settings.attach(1,"replacement") && settings.define_text(1,d)==1 && settings.read_text(1,1,text)==1 && text.length==0,"text cleanup and default on new attachment");
    // Full valid registry must fit the renderer response bound, including escaped text.
    crml::ModSettings full;d.max_bytes=255;std::memset(d.initial,'"',255);d.initial[255]=0;
    std::memset(d.label,'"',95);d.label[95]=0;std::memset(d.description,'\\',191);d.description[191]=0;
    for(uint64_t owner=1;owner<=32;++owner) {
        require(full.attach(owner,"owner-"+std::to_string(owner)),"full text owner");
        for(unsigned i=0;i<32;++i){const auto key="text-"+std::to_string(i);std::strcpy(d.key,key.c_str());require(full.define_text(owner,d)==static_cast<int>(i+1),"full text control");}
        std::strcpy(d.key,"overflow");require(full.define_text(owner,d)==-5,"shared count cap");
    }
    crml::SettingsUi full_ui(full);full_ui.enable(true);const auto page=full_ui.open_page();
    require(full_ui.exchange(std::string(crml::settings_prefix)+std::to_string(page)+"/1",body)==200 && body.size()<2*1024*1024,"full registry response bound");
}
int main() {
    try {
        text_settings();
        crml::ModSettings settings;
        require(settings.attach(1,"first",{"First \"mod\"","1.0.0","An author"}) && settings.attach(2,"second"),"attach owners");
        require(!settings.attach(3,"bad",{std::string(96,'x'),"",""}) &&
            !settings.attach(3,"bad",{"name","v1.0.0",""}),"invalid native metadata refused");
        require(!settings.attach(1,"third") && !settings.attach(3,"first"),"duplicate identity refused");
        auto d=definition();
        auto offset=d;offset.minimum=999999999;offset.maximum=1000000000;offset.step=0.01;offset.initial=offset.minimum;
        require(settings.attach(99,"precision"),"precision owner");
        require(settings.define(99,offset)==1,"large-offset definition");
        require(settings.set(99,1,offset.minimum+2*offset.step)==1,"UI-computed large-offset grid value");
        require(settings.set(99,1,offset.minimum+0.025)==-3,"genuine off-grid offset refused");
        settings.detach(99);
        require(settings.define(1,d)==1 && settings.define(2,d)==1,"owner-local handles");
        require(settings.snapshot()[0].metadata.name=="First \"mod\"","metadata copied with identity");
        require(settings.define(1,d)==-4,"duplicate key refused");
        std::array<crml_setting_value,32> values{};
        require(settings.read(1,{})==-3,"snapshot short buffer");
        require(settings.read(1,values)==1 && values[0].value==2 && values[0].revision==1,"initial snapshot");
        require(settings.set(1,1,2,1)==0 && settings.set(1,1,3,1)==1,"unchanged and changed outcomes");
        require(settings.set(1,1,4,1)==-4,"stale revision cannot overwrite");
        require(settings.set(1,2,3)==-2 && settings.set(3,1,3)==-1,"unknown handles and owners");
        require(settings.set(1,1,0.1)==-3 && settings.set(1,1,11)==-3,"grid and range enforcement");
        require(settings.set(1,1,std::numeric_limits<double>::quiet_NaN())==-3,"nonfinite value refused");
        require(settings.read(2,values)==1 && values[0].value==2,"independent owner value");
        for(int i=0;i<8;++i) {
            auto bad=d;
            if(i==0)bad.version=2;
            if(i==1)std::memset(bad.label,'x',sizeof(bad.label));
            if(i==2)bad.step=0;
            if(i==3)bad.initial=20;
            if(i==4)bad.maximum=10.1;
            if(i==5)bad.label[0]=static_cast<char>(0xff);
            if(i==6)bad.description[0]='\n';
            if(i==7)std::strcpy(bad.key,"../outside");
            require(settings.define(1,bad)==-3,"invalid descriptor refused");
        }
        auto toggle=d;toggle.kind=CRML_SETTING_BOOL;std::strcpy(toggle.key,"enabled");toggle.minimum=0;toggle.maximum=1;toggle.step=1;toggle.initial=0;
        require(settings.define(1,toggle)==2 && settings.set(1,2,0.5)==-3 && settings.set(1,2,1)==1,"boolean contract");
        auto integer=d;integer.kind=CRML_SETTING_INT;std::strcpy(integer.key,"count");integer.step=1;
        require(settings.define(1,integer)==3 && settings.set(1,3,1.5)==-3,"integer contract");
        for(int i=3;i<32;++i){auto next=d;const auto key="value-"+std::to_string(i);std::strcpy(next.key,key.c_str());require(settings.define(1,next)==i+1,"fill registry");}
        std::strcpy(d.key,"overflow");require(settings.define(1,d)==-5,"setting count bound");
        for(uint64_t i=3;i<=32;++i)require(settings.attach(i,"mod-"+std::to_string(i)),"fill owners");
        require(!settings.attach(33,"overflow"),"owner count bound");

        crml::SettingsUi ui(settings);std::string body;
        const std::string prefix(crml::settings_prefix);
        require(ui.exchange(prefix+"1/1",body)==503,"disabled endpoint");
        ui.enable(true);const auto page=ui.open_page();const auto base=prefix+std::to_string(page)+"/";
        require(ui.exchange(base+"1",body)==200 && body.find("Flight \\\"speed\\\"")!=body.npos,"JSON escaping and snapshot");
        require(ui.exchange(base+"1",body)==409,"duplicate sequence");
        require(ui.exchange(base+"2/2/1/1/4",body)==200 && body.find("\"result\":1")!=body.npos,"renderer edit applied");
        require(settings.read(2,values)==1 && values[0].value==4 && values[0].revision==2,"renderer and guest share values");
        require(ui.exchange(base+"3/2/1/1/5",body)==200 && body.find("\"result\":-4")!=body.npos,"stale renderer edit rejected");
        for(const auto suffix:{"4/2/1/0/5","4/2/1/2/nan","4/2/1/2/inf","4/2/1/2/5/extra","4/","-1","0"})
            require(ui.exchange(base+suffix,body)==400,"malformed renderer request");
        settings.detach(2);
        require(ui.exchange(base+"4/2/1/2/5",body)==200 && body.find("\"result\":-1")!=body.npos,"late edit after unload rejected");
        const auto next=ui.open_page();require(next!=page && ui.exchange(base+"5",body)==403,"old document retired");
        ui.enable(false);ui.enable(true);require(ui.exchange(prefix+std::to_string(next)+"/1",body)==403,"disable retires document identity");
        settings.detach(1);require(settings.read(1,values)==-1 && settings.snapshot().empty(),"detach cleans registered controls");
        std::cout<<"Typed settings ownership, validation, revisions and renderer protocol passed\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
