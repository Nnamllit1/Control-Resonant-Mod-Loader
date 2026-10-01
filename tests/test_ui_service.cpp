#include "ui_service.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(value) do {if(!(value)) throw std::runtime_error("UI service check failed at line "+std::to_string(__LINE__)+": " #value);} while(false)
struct Fixture {
    crml::ui::Service service;
    uint64_t page{},now{100},generation{};
    uint32_t sequence{};
    std::string body;
    Fixture() {service.enable(true);page=service.open_page();CHECK(page!=0);}
    std::string url(uint32_t screen=1,uint32_t actions=1,uint32_t ack=0) {
        return std::string(crml::ui::poll_prefix)+std::to_string(page)+"/"+
            std::to_string(++sequence)+"/"+std::to_string(screen)+"/"+
            std::to_string(actions)+"/"+std::to_string(ack);
    }
    int poll(uint32_t screen=1,uint32_t actions=1,uint32_t ack=0) {
        return service.exchange(url(screen,actions,ack),now,body);
    }
    crml_ui_state read() {
        crml_ui_state out{};CHECK(service.read_at(out,now)==1);
        generation=out.generation;return out;
    }
    uint32_t id() const {
        const auto start=body.find("\"id\":");CHECK(start!=std::string::npos);
        return static_cast<uint32_t>(std::stoul(body.substr(start+5)));
    }
    int activate(uint64_t owner=7) {return service.activate_at(owner,generation,CRML_UI_ACTION_CONTINUE,now);}
};
void parser_and_availability() {
    crml::ui::Service service;std::string body="old";crml_ui_state state{};
    CHECK(service.capabilities()==0 && service.open_page()==0);
    CHECK(service.read_at(state,100)<0 && state.size==0);
    CHECK(service.exchange("coui://base/not-crml",100,body)==0 && body.empty());
    CHECK(service.exchange("coui://base/crml/ui/v1/1/1/1/1/0",100,body)==503);
    Fixture f;
    const std::string prefix(crml::ui::poll_prefix);
    for(const auto* suffix:{"", "1/1/1/1", "1/1/1/1/0/", "1/1/1/1/0?q=1",
        "01/1/1/1/0", "+1/1/1/1/0", "-1/1/1/1/0", "0/1/1/1/0",
        "1/0/1/1/0", "1/4294967296/1/1/0", "1/1/10/1/0", "1/1/1/2/0",
        "1/1/1/1/4294967296", "18446744073709551616/1/1/1/0"})
        CHECK(f.service.exchange(prefix+suffix,100,body)==400);
    CHECK(f.service.exchange(prefix+std::string(129,'9'),100,body)==400);
    std::string nul=prefix+"1/1/1/1/0";nul.push_back('\0');
    CHECK(f.service.exchange(nul,100,body)==400);
    CHECK(f.poll()==200);const auto first=f.read();
    CHECK(first.size==CRML_UI_STATE_SIZE && first.version==CRML_UI_STATE_VERSION);
    CHECK(first.screen==1 && first.actions==1 && first.reserved==0 && first.age_ms==0);
    CHECK(first.generation!=0);
    CHECK(f.service.exchange(prefix+"1/1/1/1/0",100,body)==409);
    CHECK(f.service.exchange(prefix+"2/2/1/1/0",100,body)==409);
    CHECK(f.service.exchange(prefix+"1/2/1/1/1",100,body)==409);
    CHECK(f.service.read_at(state,1100)==1 && state.age_ms==1000);
    CHECK(f.service.read_at(state,1101)<0 && state.size==0);
    CHECK(f.service.read_at(state,99)<0 && state.size==0);
}
void delivery_and_ownership() {
    Fixture f;CHECK(f.poll()==200);f.read();
    CHECK(f.activate()==0 && f.service.submissions()==1);
    CHECK(f.poll(1,1,1)==409); // An id cannot be acknowledged before delivery.
    CHECK(f.activate(8)<0); // Single shared pending slot, including other mods.
    CHECK(f.service.acknowledgements()==0); // Queue acceptance is not completion.
    CHECK(f.poll()==200);const auto id=f.id();CHECK(id!=0);
    CHECK(f.service.acknowledgements()==0);
    CHECK(f.poll()==200 && f.id()==0); // Never redeliver on another poll.
    CHECK(f.activate()<0); // Delivered but not yet acknowledged is still pending.
    f.service.release(8);CHECK(f.activate()<0); // Different owner cannot cancel.
    CHECK(f.poll(1,1,id)==200 && f.id()==0);
    CHECK(f.service.acknowledgements()==1 && f.activate()==0);
    CHECK(f.poll(1,1,id)==200);CHECK(f.id()!=0 && f.id()!=id);
    CHECK(f.service.acknowledgements()==1); // Previous ack cannot complete next command.
    f.service.release(7);CHECK(f.activate(8)==0);
    f.service.release(8);CHECK(f.poll()==200 && f.id()==0);
}
void acknowledgements_across_screen_changes() {
    Fixture f;CHECK(f.poll()==200);f.read();CHECK(f.activate()==0);
    CHECK(f.poll()==200);const auto first=f.id();CHECK(first!=0);
    CHECK(f.poll(5,1,first)==200 && f.id()==0);
    CHECK(f.service.acknowledgements()==1); // Ack and next-screen snapshot share one poll.
    f.read();CHECK(f.activate()==0);CHECK(f.poll(5)==200);const auto next=f.id();CHECK(next>first);
    CHECK(f.poll(8)==200 && f.id()==0); // Changed screen invalidates delivery pending ack.
    CHECK(f.poll(8,1,next)==200 && f.service.acknowledgements()==1);
    f.read();CHECK(f.activate()==0);CHECK(f.poll(8,1,first)==200 && f.id()>next);
}
void boundaries_and_invalidations() {
    for(uint32_t screen=0;screen<=CRML_UI_SCREEN_MAIN_MENU;++screen) {
        Fixture f;CHECK(f.poll(screen,1)==200);const auto snapshot=f.read();
        const bool allowed=screen==1 || screen==5 || screen==8;
        CHECK(snapshot.actions==(allowed?1u:0u));CHECK((f.activate()==0)==allowed);
    }
    Fixture f;CHECK(f.poll()==200);f.read();
    CHECK(f.service.activate_at(0,f.generation,1,f.now)<0);
    CHECK(f.service.activate_at(7,0,1,f.now)<0);
    CHECK(f.service.activate_at(7,f.generation+1,1,f.now)<0);
    CHECK(f.service.activate_at(7,f.generation,2,f.now)<0);
    CHECK(f.activate()==0);
    const auto original=f.generation;
    CHECK(f.poll(1,0)==200 && f.id()==0);CHECK(f.read().generation!=original);
    CHECK(f.activate()<0);
    CHECK(f.poll(5,1)==200);f.read();CHECK(f.activate()==0);
    CHECK(f.poll(8,1)==200 && f.id()==0);f.read();CHECK(f.activate()==0);
    const auto old_page=f.page;
    f.page=f.service.open_page();CHECK(f.page!=old_page);
    crml_ui_state out{};CHECK(f.service.read_at(out,f.now)<0);
    CHECK(f.poll()==200 && f.id()==0);CHECK(f.read().generation!=original);
    CHECK(f.activate()==0);f.service.enable(false);
    CHECK(f.service.capabilities()==0 && f.activate()<0);
    f.service.enable(true);CHECK(f.service.read_at(out,f.now)<0);
    CHECK(f.poll()==200 && f.id()==0);
}
void expiration() {
    Fixture f;CHECK(f.poll()==200);f.read();CHECK(f.activate()==0);
    f.now=2100;CHECK(f.poll()==200 && f.id()!=0); // Exactly 2 seconds remains valid.
    f.now=2101;CHECK(f.poll()==200 && f.id()==0);f.read();CHECK(f.activate()==0);
    f.service.release(7);
    f.now=99;CHECK(f.service.activate_at(7,f.generation,1,f.now)<0);
    f.now=2200;CHECK(f.poll()==200);f.read();CHECK(f.activate()==0);
    f.now=4301;CHECK(f.poll()==200 && f.id()==0); // Undelivered command expires too.
    f.read();f.now=5302;CHECK(f.activate()<0); // Freshness is independent of expiry.
}
void presentation_leases() {
    Fixture f;CHECK(f.poll()==200);f.read();
    auto present=[&](uint64_t owner,std::string_view name,bool hidden=true,uint32_t ttl=750) {
        return f.service.present_at(owner,f.generation,2,name,hidden,ttl,f.now);
    };
    CHECK(present(7,"splash")==0);
    CHECK(f.poll()==200 && f.body.find("\"name\":\"splash\"")!=f.body.npos);
    CHECK(present(8,"splash")==-2);
    CHECK(present(8,"other-screen")==0);
    f.service.release(7);
    CHECK(f.poll()==200 && f.body.find("splash")==f.body.npos && f.body.find("other-screen")!=f.body.npos);
    CHECK(present(8,"other-screen",false,0)==0);
    CHECK(f.poll()==200 && f.body.find("\"leases\":[]")!=f.body.npos);
    CHECK(present(7,"splash")==0);
    CHECK(f.poll(1,0)==200 && f.body.find("splash")!=f.body.npos); // Action latch does not revoke presentation.
    f.read();f.now+=749;CHECK(f.poll(1,0)==200 && f.body.find("\"ttl_ms\":1")!=f.body.npos);
    ++f.now;CHECK(f.poll(1,0)==200 && f.body.find("splash")==f.body.npos);
    f.read();CHECK(present(7,"splash")==0);
    CHECK(f.poll(3,0)==200 && f.body.find("splash")==f.body.npos); // New screen revokes before publication.
    f.read();CHECK(present(7,"splash")==0);
    CHECK(f.service.present_at(7,f.generation+1,2,"splash",true,750,f.now)<0);
    for(auto name:{"", "a b", "#splash", "a\"b", "a:b", "a.b"}) CHECK(present(7,name)<0);
    CHECK(present(7,std::string(65,'a'))<0);
    CHECK(f.service.present_at(7,f.generation,0,"splash",true,750,f.now)<0);
    CHECK(present(7,"splash",true,1001)<0 && present(7,"splash",true,0)<0 && present(7,"splash",false,1)<0);
    CHECK(f.service.present_at(0,f.generation,2,"splash",true,750,f.now)<0);
    f.service.release(7);
    for(int i=0;i<8;++i) CHECK(present(7,"node"+std::to_string(i))==0);
    CHECK(present(7,"overflow")==-2);
    CHECK(f.poll(3,0)==200 && f.body.size()<2048);
    f.page=f.service.open_page();CHECK(f.poll(3,0)==200 && f.body.find("node0")==f.body.npos);
    f.read();CHECK(present(7,"splash")==0);f.service.enable(false);f.service.enable(true);
    CHECK(f.poll(3,0)==200 && f.body.find("splash")==f.body.npos);
}
}
int main() {
    try {parser_and_availability();delivery_and_ownership();acknowledgements_across_screen_changes();boundaries_and_invalidations();expiration();presentation_leases();
        std::cout<<"UI service parsing, lifetime, generation, bounded delivery and acknowledgement checks passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
