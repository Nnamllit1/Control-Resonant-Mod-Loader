#include "mod_feedback.h"
#include <array>
#include <iostream>
#include <stdexcept>

void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {
    try {
        uint64_t now{};crml::ModFeedback feedback([&]{return now;});
        for(uint64_t i=1;i<=32;++i)require(feedback.attach(i,"mod-"+std::to_string(i)),"attach owner");
        require(!feedback.attach(33,"full") && !feedback.attach(1,"duplicate"),"bounded identities");
        require(!feedback.available() && feedback.show(1,"hello",0,1000)==-1,"renderer unavailable");
        feedback.enable_renderer(true);const auto page=feedback.open_page();require(page!=0,"page issued");
        require(feedback.show(1,"bad\ntext",0,1000)==-3 && feedback.show(1,std::string(241,'x'),0,1000)==-3,"bounded plain text");
        require(feedback.show(1,std::string(1,char(0xff)),0,1000)==-3,"invalid UTF8 rejected");
        require(feedback.show(1,"text",4,1000)==-3 && feedback.show(1,"text",0,999)==-3,"invalid arguments rejected");
        const auto first=feedback.show(1,"hello \"world\"",0,1000);require(first>0,"receipt issued");
        require(feedback.status(1,first)==CRML_FEEDBACK_QUEUED,"accepted is queued");
        require(feedback.status(2,first)==-2 && feedback.dismiss(2,first)==-2,"foreign receipt rejected");
        require(feedback.show(1,"again",0,1000)==-2,"one active message per owner");
        std::vector<crml::ModFeedback::Message> messages;
        std::array ack{static_cast<uint64_t>(first)};
        require(feedback.poll(page,1,ack,messages)==200 && messages.size()==1,"poll snapshot");
        require(feedback.status(1,first)==CRML_FEEDBACK_QUEUED,"unoffered ticket cannot be acknowledged");
        require(feedback.poll(page,2,ack,messages)==200 && feedback.status(1,first)==CRML_FEEDBACK_PRESENTED,"offered ticket acknowledged");
        for(uint64_t i=2;i<=4;++i)require(feedback.show(i,"shared screen",0,1000)>0,"shared slots");
        require(feedback.show(5,"full",0,1000)==-2,"global capacity refused");
        require(feedback.dismiss(1,first)==1 && feedback.dismiss(1,first)==0,"explicit dismissal idempotent");
        require(feedback.show(1,"too soon",0,1000)==-4,"per-owner publish rate");
        now=1000;
        require(feedback.status(2,2)==CRML_FEEDBACK_EXPIRED_UNPRESENTED,"undelivered expiry distinguished");
        const auto second=feedback.show(1,"next",1,1000);require(second>0,"new receipt after cooldown");
        require(feedback.status(1,first)==-2,"superseded receipt stale");
        require(feedback.poll(page,3,{},messages)==200 && messages.size()==1,"expired messages disappear");
        ack[0]=static_cast<uint64_t>(second);
        const auto replacement=feedback.open_page();
        require(feedback.poll(page,4,ack,messages)==403,"old page cannot acknowledge");
        require(feedback.poll(replacement,1,ack,messages)==200 && feedback.status(1,second)==CRML_FEEDBACK_QUEUED,"new page must first receive ticket");
        require(feedback.poll(replacement,2,ack,messages)==200,"replacement acknowledgement");
        now=2000;require(feedback.status(1,second)==CRML_FEEDBACK_EXPIRED_PRESENTED,"delivered expiry distinguished");
        const auto third=feedback.show(1,"teardown",2,10000);require(third>0,"third receipt");
        feedback.enable_renderer(false);
        require(feedback.status(1,third)==CRML_FEEDBACK_CANCELLED && !feedback.available(),"renderer stop cancels active receipts");
        feedback.enable_renderer(true);const auto current=feedback.open_page();now=3000;
        const auto fourth=feedback.show(1,"copied text",3,1000);require(fourth>0,"new renderer message");
        std::string body;
        const auto url=std::string(crml::feedback_prefix)+std::to_string(current)+"/";
        require(feedback.exchange(url+"1",body)==200 && body.find("copied text")!=body.npos,"renderer JSON protocol");
        require(feedback.exchange(url+"1",body)==409 && body.empty(),"replay refused");
        require(feedback.exchange(url+"2/1,1",body)==400,"duplicate acknowledgements refused");
        require(feedback.exchange(url+"2/",body)==400 && feedback.exchange(url+"2/1,",body)==400,"malformed URL rejected");
        feedback.detach(1);
        require(feedback.exchange(url+"2",body)==200 && body=="{\"messages\":[]}","unload clears presentation");
        require(feedback.status(1,fourth)==-1,"unloaded owner gone");
        const auto late=feedback.show(2,"late delivery",0,1000);require(late>0,"late receipt");
        now=3900;require(feedback.poll(current,3,{},messages)==200,"offer near deadline");
        now=4150;require(feedback.status(2,late)==CRML_FEEDBACK_EXPIRED_UNPRESENTED,"expiry before acknowledgement");
        ack[0]=static_cast<uint64_t>(late);
        require(feedback.poll(current,4,ack,messages)==200 && messages.empty(),"late acknowledgement does not redisplay");
        require(feedback.status(2,late)==CRML_FEEDBACK_EXPIRED_PRESENTED,"late renderer evidence retained");
        std::cout<<"Feedback ownership, bounds, delivery receipts, expiry, rendering protocol and cleanup passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
