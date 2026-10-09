#include "action_rules.h"
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
using namespace crml::action_rules;
Identity context{123,456,7};
uint64_t now=1000;
bool present=true;
bool resolve(Identity& out) noexcept {out=context;return present;}
uint64_t clock_now() noexcept {return now;}
void require(bool b,const char* why) {if(!b) throw std::runtime_error(why);}
}
int main() {
    try {
        static_assert(sizeof(crml_action_rule_state)==32);
        Service service(&resolve,&clock_now);
        crml_action_rule_state out{};
        require(!service.capabilities() && service.action_rule_set(1,1,1)==-1,"unavailable");
        service.available(true);
        require(service.capabilities()==CRML_CAP_ACTION_RULES,"availability");
        require(service.action_rule_set(0,1,1)==-3 && service.action_rule_set(1,32,1)==-3 &&
                service.action_rule_set(1,1,2)==-3 && service.action_rule_set(1,0,1)==-3,"validate masks and owner");
        present=false;require(service.action_rule_set(1,1,1)==-5,"no context");present=true;
        require(service.action_rule_set(1,CRML_RULE_ACTION_MELEE,1)==1,"first lease");
        require(service.pending(now),"cheap pending gate sees request");
        require(service.action_rule_set(2,CRML_RULE_ACTION_DASH,1)==1 && service.allowed(context,now)==0x10001,"owners compose");
        require(service.action_rule_read(1,out)==1 && out.version==1 && out.state==CRML_RULE_LEASED &&
                out.remaining_ms==500 && out.requested_actions==1 && out.supported_actions==31,"owner snapshot");
        service.release(1);require(service.allowed(context,now)==0x10000,"owner release preserves peer");
        require(service.action_rule_read(1,out)==1 && out.state==CRML_RULE_IDLE,"released state");
        auto other=context;++other.entity;require(!service.allowed(other,now),"entity binding");
        other=context;++other.world;require(!service.allowed(other,now),"world binding");
        other=context;++other.lifetime;require(!service.allowed(other,now),"world address reuse");
        ++context.lifetime;
        require(service.action_rule_read(2,out)==1 && out.state==CRML_RULE_CONTEXT_CHANGED && !out.remaining_ms,"lifetime snapshot");
        --context.lifetime;
        now=1500;require(!service.pending(now) && !service.allowed(context,now),"expiry at boundary");
        require(service.action_rule_read(2,out)==1 && out.state==CRML_RULE_EXPIRED,"expired request is not permission");
        for(unsigned owner=1;owner<=32;++owner) require(service.action_rule_set(owner,31,1)==1,"bounded owners");
        require(service.action_rule_set(33,1,1)==-2,"full");
        require(service.action_rule_set(1,2,1)==1,"same owner can renew when full");
        service.release(4);require(service.action_rule_set(33,1,1)==1,"reclaim released slot");
        now+=500;require(service.action_rule_set(34,4,1)==1,"reclaim expired slot");
        require(service.allowed(context,now)==0x10,"only live owner applies");
        require(service.action_rule_set(34,0,0)==0 && !service.pending(now) && !service.allowed(context,now),"explicit release");
        std::atomic<bool> finished{};
        std::atomic<uint64_t> reads{};
        std::thread reader([&] {while(!finished.load()) {
            const auto value=service.allowed(context,now);
            if(value!=0 && value!=0x10033) std::terminate();
            ++reads;
        }});
        for(unsigned i=0;i<4000;++i) {service.action_rule_set(99,31,1);service.release(99);}
        finished=true;reader.join();
        service.available(false);out.version=99;
        require(!service.allowed(context,now) && service.action_rule_read(1,out)==-1 && !out.version,"stop clears");
        service.available(true);require(!service.allowed(context,now),"restart never resurrects");
        now=UINT64_MAX;require(service.action_rule_set(1,1,1)==-3,"deadline overflow");
        std::cout<<"Action rules: owner composition, expiry, identity, capacity, concurrency and cleanup passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
