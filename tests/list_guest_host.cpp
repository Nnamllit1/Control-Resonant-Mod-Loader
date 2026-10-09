#include "runtime.h"
#include "mod_lists.h"
#include "mod_settings.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void browser_test(const wchar_t* mods) {
    uint64_t now{};crml::ModLists lists([&]{return now;});crml::ModSettings settings;
    lists.enable_renderer(true);
    crml::Runtime runtime([](const std::string& text){std::cout<<text<<'\n';},nullptr,nullptr,{},nullptr,&settings,nullptr,[&]{return now;},nullptr,nullptr,&lists);
    runtime.load(mods);require(runtime.active()==1&&!runtime.failures(),"compiled list browser loaded");
    const auto tick=[&]{runtime.tick(.2f);now+=200;require(runtime.active()==1&&!runtime.failures(),"compiled list browser trapped");};
    const auto group=[&]{const auto groups=lists.snapshot();require(groups.size()==1,"example page absent");return groups[0];};
    const auto setting=[&](const std::string& key){for(const auto& g:settings.snapshot())for(const auto& i:g.items)if(i.definition.key==key)return std::pair{g.owner,i.state};throw std::runtime_error("missing example setting "+key);};
    const auto search=[&](const std::string& value){const auto [owner,state]=setting("search");require(settings.set_text(owner,state.handle,value,state.revision)>0,"set search");};
    const auto activate=[&](uint64_t row,int expected=200){const auto g=group();require(lists.activate(g.owner,g.revision,row)==expected,"example activation");};
    const auto selected=[&](uint64_t id){const auto g=group();for(uint32_t i=0;i<g.page.row_count;++i)if(g.page.rows[i].id==id)return (g.page.rows[i].flags&CRML_LIST_ROW_SELECTED)!=0;return false;};
    tick();auto g=group();require(g.page.row_count==10&&g.page.rows[0].id==1&&g.page.rows[7].id==8,"first page stable item IDs");
    activate(1001,409);activate(4);tick();require(selected(4),"Blue selection copied");
    activate(1002);tick();g=group();require(g.page.rows[0].id==9&&g.page.rows[7].id==16,"next page");
    activate(1001);tick();require(selected(4),"selection retained across paging");
    search("iR");tick();g=group();require(g.page.row_count==4&&g.page.rows[0].id==3&&g.page.rows[1].id==18,"case-insensitive filtering across original pages");
    search("AsH");tick();require(group().page.row_count==3&&group().page.rows[0].id==2,"single result filter");
    activate(2);search("BLUE");tick();require(selected(4),"settings edit discards accepted event from previous view");
    const auto [owner,font]=setting("font_scale");require(settings.set(owner,font.handle,1.5,font.revision)>0,"set font");
    tick();require(group().page.font_scale==1.5f&&selected(4),"font applies without changing selected ID");
    search("no-such-entry");tick();g=group();require(g.page.row_count==2&&!g.page.rows[0].flags&&!g.page.rows[1].flags,"empty search leaves disabled navigation");
    search("");tick();g=group();const auto old=g;
    for(unsigned i=0;i<5;++i)activate(1002);
    tick();require(group().page.rows[0].id==33,"four actions per tick bounded");
    tick();require(group().page.rows[0].id==33,"fifth accepted old-revision action ignored");
    require(lists.activate(old.owner,old.revision,1002)==409,"rendered old revision rejected");activate(1002,409);
    activate(33);lists.enable_renderer(false);tick();require(lists.snapshot().empty(),"renderer disable clears page and pending action");
    lists.enable_renderer(true);tick();require(group().page.rows[0].id==33&&!selected(33),"renderer recovery republishes without replaying canceled action");
    search("BLUE");tick();require(selected(4),"previous selection survives renderer recovery");
    runtime.shutdown();require(lists.snapshot().empty()&&!runtime.failures(),"compiled example shutdown clears list");
    std::cout<<"Compiled list browser search, paging, selection, font, stale actions and renderer lifecycle passed\n";
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3&&std::wstring_view(argv[2])==L"--browser"){browser_test(argv[1]);return 0;}
        require(argc==4,"Expected mods-A mods-B-or-dash renderer-enabled");
        uint64_t now{};crml::ModLists lists([&]{return now;});
        lists.enable_renderer(std::wstring_view(argv[3])==L"1");
        auto log=[](const std::string& text){std::cout<<text<<'\n';};
        crml::Runtime a(log,nullptr,nullptr,{},nullptr,nullptr,nullptr,[&]{return now;},nullptr,nullptr,&lists);
        crml::Runtime b(log,nullptr,nullptr,{},nullptr,nullptr,nullptr,[&]{return now;},nullptr,nullptr,&lists);
        a.load(argv[1]);if(std::wstring_view(argv[2])!=L"-")b.load(argv[2]);
        std::map<std::string,crml::ModLists::Group> saved;
        for(std::string line;std::getline(std::cin,line);){
            std::istringstream in(line);std::string op;in>>op;
            if(op=="pages") {size_t n{};in>>n;require(lists.snapshot().size()==n,"page count: "+line);}
            else if(op=="save") {
                std::string key,id;in>>key>>id;const auto pages=lists.snapshot();
                const auto found=std::find_if(pages.begin(),pages.end(),[&](const auto& p){return p.id==id;});
                require(found!=pages.end(),"missing page: "+line);saved[key]=*found;
            }else if(op=="title" || op=="label") {
                std::string value;in>>value;const auto pages=lists.snapshot();
                require(std::any_of(pages.begin(),pages.end(),[&](const auto& p){return op=="title"?value==p.page.title:p.page.row_count&&value==p.page.rows[0].label;}),"missing copied text: "+line);
            }else if(op=="activate" || op=="fill") {
                std::string key;uint64_t row{};int delta{},expected{};unsigned count=1;
                in>>key>>row>>delta>>expected;if(op=="fill")in>>count;
                const auto& page=saved.at(key);
                for(unsigned i=0;i<count;++i)require(lists.activate(page.owner,page.revision+delta,row)==expected,"activation result: "+line);
            }else if(op=="detach") {std::string key;in>>key;lists.detach(saved.at(key).owner);}
            else if(op=="renderer") {unsigned enabled{};in>>enabled;lists.enable_renderer(enabled!=0);}
            else if(op=="clock") {in>>now;}
            else if(op=="tick_a" || op=="tick_b") {unsigned ms{};in>>ms;now+=ms;(op=="tick_a"?a:b).tick(ms/1000.f);}
            else if(op=="shutdown_a" || op=="shutdown_b") {(op=="shutdown_a"?a:b).shutdown();}
            else if(op=="load_b_from_a") {b.load(argv[1]);}
            else if(op=="active_a" || op=="active_b") {size_t n{};in>>n;require((op=="active_a"?a:b).active()==n,"active count: "+line);}
            else if(op=="failures_a" || op=="failures_b") {size_t n{};in>>n;require((op=="failures_a"?a:b).failures()==n,"failure count: "+line);}
            else throw std::runtime_error("Unknown command "+op);
            require(!in.fail(),"Invalid command "+line);
        }
        std::cout<<"List Wasm scenario passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
