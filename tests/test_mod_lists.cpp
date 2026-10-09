#include "mod_lists.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
crml_list_page page() {
    crml_list_page p{};p.version=1;p.size=sizeof(p);p.font_scale=1;p.row_count=2;
    strcpy_s(p.title,"Choices <safe>");
    p.rows[0].id=UINT64_MAX;p.rows[0].flags=CRML_LIST_ROW_ENABLED|CRML_LIST_ROW_SELECTED;
    strcpy_s(p.rows[0].label,"First \xf0\x9f\x98\x80");strcpy_s(p.rows[0].detail,"Plain \"text\"");
    p.rows[1].id=2;strcpy_s(p.rows[1].label,"Unavailable");return p;
}
bool zero(const crml_list_event& event){const crml_list_event empty{};return !std::memcmp(&event,&empty,sizeof(event));}
int main() {
    try {
        uint64_t now{};crml::ModLists lists([&]{return now;});auto p=page();crml_list_event event{};
        require(!lists.attach(0,"bad")&&!lists.attach(1,"Upper")&&!lists.attach(1,"../bad"),"owner IDs validated");
        for(uint64_t i=1;i<=32;++i)require(lists.attach(i,"mod-"+std::to_string(i)),"owner attach");
        require(!lists.attach(33,"extra")&&!lists.attach(1,"duplicate"),"owner capacity");
        require(lists.publish(1,p)==-1&&!lists.available()&&lists.snapshot().empty(),"renderer unavailable");
        lists.enable_renderer(true);require(lists.available(),"renderer enabled");
        const auto first=lists.publish(1,p);require(first>0,"publish returns revision");
        auto groups=lists.snapshot();require(groups.size()==1&&groups[0].owner==1&&groups[0].revision==static_cast<uint64_t>(first),"copied snapshot");
        p.rows[0].label[0]='X';require(groups[0].page.rows[0].label[0]=='F',"guest page copied");
        require(lists.publish(1,p)==-4,"initial rate limit including clock zero");now=100;
        auto bad=p;bad.version=2;require(lists.publish(1,bad)==-3,"version rejected");
        bad=p;bad.size--;require(lists.publish(1,bad)==-3,"size rejected");
        bad=p;bad.row_count=33;require(lists.publish(1,bad)==-3,"row count checked before access");
        bad=p;bad.rows[0].id=0;require(lists.publish(1,bad)==-3,"zero row ID rejected");
        bad=p;bad.rows[1].id=bad.rows[0].id;require(lists.publish(1,bad)==-3,"duplicate row ID rejected");
        bad=p;bad.rows[1].flags=2;require(lists.publish(1,bad)==-3,"multiple selected rejected");
        bad=p;bad.rows[0].flags=4;require(lists.publish(1,bad)==-3,"unknown flags rejected");
        bad=p;bad.rows[0].reserved=1;require(lists.publish(1,bad)==-3,"reserved row bits rejected");
        for(float scale:{.74f,1.51f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
            bad=p;bad.font_scale=scale;require(lists.publish(1,bad)==-3,"font bounds");}
        bad=p;bad.title[0]=0;require(lists.publish(1,bad)==-3,"required title");
        bad=p;bad.rows[0].label[0]=0;require(lists.publish(1,bad)==-3,"required label");
        bad=p;std::memset(bad.rows[0].detail,'x',sizeof(bad.rows[0].detail));require(lists.publish(1,bad)==-3,"terminator required");
        for(const auto* text:{"\xff","\xc0\xaf","\xed\xa0\x80","a\nb","a\x7f","\xc2\x85","\xe2\x80\xa8","\xe2\x80\xa9"}){
            bad=p;strcpy_s(bad.rows[0].detail,text);require(lists.publish(1,bad)==-3,"invalid UTF8 and controls rejected");}
        require(lists.snapshot()[0].revision==static_cast<uint64_t>(first),"all invalid publishes leave original intact");
        const auto second=lists.publish(1,p);require(second>first,"replacement revision");
        require(lists.activate(1,first,UINT64_MAX)==409,"old revision rejected");
        require(lists.activate(0,second,1)==400&&lists.activate(1,0,1)==400&&lists.activate(1,second,0)==400,"malformed identities");
        require(lists.activate(33,second,1)==404&&lists.activate(2,second,UINT64_MAX)==409,"foreign and hidden owner rejected");
        require(lists.activate(1,second,2)==409&&lists.activate(1,second,3)==404,"disabled and absent rows rejected");
        for(int i=0;i<16;++i)require(lists.activate(1,second,UINT64_MAX)==200,"bounded activation accepted");
        require(lists.activate(1,second,UINT64_MAX)==429,"full queue rejects without dropping");
        require(lists.next(2,event)==0&&zero(event),"other owner cannot read queued events");
        uint64_t sequence{};
        for(int i=0;i<16;++i){require(lists.next(1,event)==1&&event.version==1&&!event.flags&&event.revision==static_cast<uint64_t>(second)&&event.row_id==UINT64_MAX&&event.sequence>sequence,"FIFO accepted events once");sequence=event.sequence;}
        require(lists.next(1,event)==0&&zero(event),"destructive read empties queue and zeroes output");
        now=200;p.rows[31].reserved=99;p.rows[0].label[95]='x';
        require(lists.publish(1,p)==second,"ignored inactive rows and bytes after NUL retain revision");
        require(lists.activate(1,second,UINT64_MAX)==200,"queue after drain");
        now=300;p.font_scale=1.5f;const auto third=lists.publish(1,p);require(third>second,"font revision");
        require(lists.next(1,event)==1&&event.revision==static_cast<uint64_t>(second),"accepted event retains older model identity");
        require(lists.activate(1,third,UINT64_MAX)==200,"current action accepted");
        require(lists.hide(1)==1&&lists.hide(1)==0&&lists.next(1,event)==0&&zero(event),"hide cancels page and actions");
        require(lists.publish(1,p)==-4,"hide cannot bypass rate");
        now=400;const auto fourth=lists.publish(1,p);require(fourth>third,"same content after hide gets new revision");
        require(lists.activate(1,third,UINT64_MAX)==409,"no stale revival after hide");
        auto p2=page();p2.font_scale=.75f;const auto other=lists.publish(2,p2);require(other>fourth,"other owner revision distinct");
        require(lists.activate(2,fourth,UINT64_MAX)==409&&lists.activate(2,other,UINT64_MAX)==200,"owner/revision isolation");
        lists.cancel(1);require(lists.snapshot().size()==1&&lists.next(2,event)==1,"cancel isolated");
        require(lists.activate(2,other,UINT64_MAX)==200,"second pending action");lists.clear();
        require(lists.snapshot().empty()&&lists.next(2,event)==0&&zero(event),"explicit clear removes all actions and pages");
        now=500;p.row_count=0;const auto empty=lists.publish(1,p);require(empty>fourth&&lists.snapshot()[0].page.row_count==0,"empty page supported");
        require(lists.activate(1,empty,UINT64_MAX)==404,"empty page rejects activation");
        lists.enable_renderer(false);require(!lists.available()&&lists.snapshot().empty()&&lists.next(1,event)==-1&&zero(event)&&lists.activate(1,empty,1)==503,"renderer stop clears and gates");
        lists.enable_renderer(true);require(lists.snapshot().empty(),"renderer restart does not revive pages");
        now=600;p=page();const auto before_detach=lists.publish(1,p);lists.detach(1);
        require(lists.next(1,event)==-1&&zero(event)&&lists.hide(1)==-1,"detached owner unavailable");
        require(lists.attach(1,"mod-1"),"reattach test identity");const auto after_detach=lists.publish(1,p);
        require(after_detach>before_detach&&lists.activate(1,before_detach,UINT64_MAX)==409,"detach never reuses revisions");
        now=599;require(lists.publish(1,p)==-4,"backwards clock rate guard");
        now=700;p.row_count=32;
        for(uint32_t i=0;i<32;++i){p.rows[i]={};p.rows[i].id=i+1;p.rows[i].flags=1;std::memset(p.rows[i].label,'x',95);std::memset(p.rows[i].detail,'y',191);}
        require(lists.publish(1,p)>0,"maximum copied page and byte lengths accepted");
        std::cout<<"List ABI, copied pages, UTF8 bounds, revisions, FIFO backpressure and owner lifecycle passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
