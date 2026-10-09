#include "mod_lists.h"
#include "settings_ui.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error("settings/list check line "+std::to_string(__LINE__)+": " #x);}while(false)
int main() {
    try {
        uint64_t now{};crml::ModSettings settings;crml::ModLists lists([&]{return now;});
        crml::SettingsUi ui(settings,&lists);ui.enable(true);lists.enable_renderer(true);
        CHECK(settings.attach(1,"mixed") && lists.attach(1,"mixed") && lists.attach(2,"list-only"));
        crml_setting_definition setting{1,CRML_SETTING_BOOL,"enabled","Enabled","",1,0,1,1};
        CHECK(settings.define(1,setting)==1);
        crml_list_page list{};list.version=1;list.size=sizeof(list);list.font_scale=1;list.row_count=2;
        strcpy_s(list.title,"Choices");list.rows[0].id=UINT64_MAX;list.rows[0].flags=CRML_LIST_ROW_ENABLED;
        strcpy_s(list.rows[0].label,"Safe \"label\"");strcpy_s(list.rows[0].detail,"<b>Plain text</b>");
        list.rows[1].id=2;strcpy_s(list.rows[1].label,"Disabled");
        const auto revision=lists.publish(1,list);CHECK(revision>0 && lists.publish(2,list)>0);
        const auto page=ui.open_page();uint64_t sequence{};std::string body;
        const auto base=std::string(crml::settings_prefix)+std::to_string(page)+"/";
        const auto request=[&](std::string_view suffix){return ui.exchange(base+std::to_string(++sequence)+std::string(suffix),body);};
        CHECK(request("")==200 && body.find("\"list\":{")!=body.npos && body.find("\"items\":[]")!=body.npos);
        CHECK(body.find("18446744073709551615")!=body.npos && body.find("Safe \\\"label\\\"")!=body.npos);
        const auto activate="/1/list/"+std::to_string(revision)+"/18446744073709551615";
        CHECK(request(activate)==200 && body.find("\"result\":1")!=body.npos);
        CHECK(ui.exchange(base+std::to_string(sequence)+activate,body)==409);
        crml_list_event event{};CHECK(lists.next(1,event)==1 && event.row_id==UINT64_MAX && lists.next(1,event)==0);
        CHECK(request("/1/list/"+std::to_string(revision)+"/2")==200 && body.find("\"result\":-4")!=body.npos);
        now=100;strcpy_s(list.title,"Changed");CHECK(lists.publish(1,list)>revision);
        CHECK(request(activate)==200 && body.find("\"result\":-4")!=body.npos && lists.next(1,event)==0);
        const auto current=lists.snapshot()[0].revision;
        const auto fresh="/1/list/"+std::to_string(current)+"/18446744073709551615";
        for(unsigned i=0;i<16;++i)CHECK(request(fresh)==200 && body.find("\"result\":1")!=body.npos);
        CHECK(request(fresh)==200 && body.find("\"result\":-6")!=body.npos);
        for(unsigned i=0;i<16;++i)CHECK(lists.next(1,event)==1);
        CHECK(lists.next(1,event)==0);
        CHECK(ui.open_page()!=page && request(fresh)==403 && lists.next(1,event)==0);
        lists.hide(2);lists.hide(1);const auto newpage=ui.open_page();
        CHECK(ui.exchange(std::string(crml::settings_prefix)+std::to_string(newpage)+"/1",body)==200);
        CHECK(body.find("list-only")==body.npos && body.find("mixed")!=body.npos && body.find("\"list\":")==body.npos);
        // Real worst-case union:32 settings owners and32 disjoint list owners.
        crml::ModSettings large_settings;crml::ModLists large_lists;large_lists.enable_renderer(true);
        for(uint64_t owner=1;owner<=32;++owner) {
            CHECK(large_settings.attach(owner,"setting-"+std::to_string(owner)));
            for(unsigned n=0;n<32;++n) {
                crml_text_setting_definition d{};d.version=1;d.max_bytes=255;
                strcpy_s(d.key,("text-"+std::to_string(n)).c_str());
                std::memset(d.label,'"',sizeof(d.label)-1);std::memset(d.description,'"',sizeof(d.description)-1);std::memset(d.initial,'"',sizeof(d.initial)-1);
                CHECK(large_settings.define_text(owner,d)>0);
            }
            CHECK(large_lists.attach(owner+32,"list-"+std::to_string(owner)));
            crml_list_page p{};p.version=1;p.size=sizeof(p);p.font_scale=1.5f;p.row_count=32;
            std::memset(p.title,'"',sizeof(p.title)-1);
            for(unsigned n=0;n<32;++n){p.rows[n].id=UINT64_MAX-n;p.rows[n].flags=1;std::memset(p.rows[n].label,'"',95);std::memset(p.rows[n].detail,'"',191);}
            CHECK(large_lists.publish(owner+32,p)>0);
        }
        crml::SettingsUi large(large_settings,&large_lists);large.enable(true);
        CHECK(large.exchange(std::string(crml::settings_prefix)+std::to_string(large.open_page())+"/1",body)==200);
        size_t owners{},offset{};while((offset=body.find("\"owner\":",offset))!=body.npos){++owners;++offset;}
        CHECK(owners==64 && body.size()<2*1024*1024);
        std::cout<<"Merged settings/list transport, actions, stale pages, queue capacity and bounded64owner catalog passed; bytes="<<body.size()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
