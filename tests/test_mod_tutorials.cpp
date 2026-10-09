#include "mod_tutorials.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <fstream>
using namespace crml;
void require(bool b,const char* message) {if(!b)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    try {
        ModTutorials s;ModTutorials::Request r;
        require(s.attach(1) && s.attach(2) && !s.attach(1),"owner isolation");
        require(s.show(1,0,"Title","Body",1000)==-1,"unavailable before native admission");
        s.enable(0,true);s.enable(1,true);
        require(!s.available(CRML_TUTORIAL_PROMPT),"prompt unavailable before native input hook");
        s.enable(CRML_TUTORIAL_PROMPT,true);
        require(s.show(1,CRML_TUTORIAL_PROMPT,"Prompt","Body",999)==-3 &&
            s.show(1,CRML_TUTORIAL_PROMPT,"Prompt","Body",30001)==-3,
            "prompt duration bounds");
        ModTutorials dynamic;dynamic.enable(CRML_TUTORIAL_HINT,true);
        dynamic.enable(CRML_TUTORIAL_PROMPT,true);
        require(dynamic.attach(1) && dynamic.attach(2),"dynamic owners");
        const auto first_dynamic=dynamic.show(1,CRML_TUTORIAL_PROMPT,"Prompt","Body",2000);
        const auto second_dynamic=dynamic.show(2,CRML_TUTORIAL_HINT,"Hint","Body",2000);
        require(first_dynamic>0 && second_dynamic>first_dynamic && dynamic.take_dynamic(r) &&
            r.ticket==first_dynamic,"shared dynamic channel uses ticket order");
        require(!dynamic.take_dynamic(r),"hint cannot overlap active prompt");
        dynamic.report(first_dynamic,CRML_TUTORIAL_DISMISSED);
        require(dynamic.take_dynamic(r) && r.ticket==second_dynamic,"hint follows prompt retirement");
        dynamic.report(second_dynamic,CRML_TUTORIAL_DISMISSED);
        require(s.show(1,0,"Title","Body",999)==-3 && s.show(1,1,"Title","Body",1000)==-3,"kind duration bounds");
        require(s.show(1,0,"Title",std::string("x\0y",3),1000)==-3 && s.show(1,0,"Title","\xff",1000)==-3,"invalid guest text");
        std::string title="Copied title",body="Copied body";
        const auto a=s.show(1,0,title,body,1000);title[0]='X';body[0]='X';
        const auto b=s.show(2,0,"Other","Other",1000);
        require(a>0 && b>a && s.status(2,a)==-2 && s.dismiss(2,a)==-2,"foreign tickets rejected");
        require(s.take(0,r) && r.ticket==a && r.title=="Copied title" && r.body=="Copied body","copied FIFO native handoff");
        require(!s.take(0,r),"one native owner per presentation kind");
        s.report(a,CRML_TUTORIAL_PRESENTED);require(s.status(1,a)==CRML_TUTORIAL_PRESENTED,"native presentation acknowledgement");
        require(s.dismiss(1,a)==1 && s.status(1,a)==CRML_TUTORIAL_CANCELLING && s.cancelled(a),"claimed cancellation waits for native retirement");
        s.report(a,CRML_TUTORIAL_PRESENTED);require(s.status(1,a)==CRML_TUTORIAL_CANCELLING,"late presentation cannot undo cancellation");
        s.detach(1);require(!s.attach(1) && !s.take(0,r),"detached native ownership remains until cleanup");
        s.report(a,CRML_TUTORIAL_CANCELLED);require(s.attach(1) && s.take(0,r) && r.ticket==b,"native cleanup releases detached slot");
        s.report(b,CRML_TUTORIAL_DISMISSED);s.report(b,CRML_TUTORIAL_PRESENTED);
        require(s.status(2,b)==CRML_TUTORIAL_DISMISSED,"terminal receipts cannot resurrect");
        const auto c=s.show(1,1,"Panel","Body",0);s.dismiss(1,c);
        require(s.status(1,c)==CRML_TUTORIAL_CANCELLED && !s.take(1,r),"unclaimed cancellation needs no native transition");
        const auto d=s.show(1,1,"Panel","Body",0);require(s.take(1,r),"panel claim");
        s.enable(1,false);require(s.cancelled(d) && s.status(1,d)==CRML_TUTORIAL_CANCELLING,"disabling backend is not disposal acknowledgement");
        require(tutorial_body_markup("<&\"'>\nx") == "&lt;&amp;&quot;&#39;&gt;<br>x","plain body cannot inject stock HTML");
        const std::string image="coui://base/textures/uiresources/UI/streamed/example.png";
        for(const auto bad:{"https://host/a.png","file:///a.png","coui://base/crml/settings/a.png",
            "coui://base/textures/uiresources/../secret.png","coui://base/textures/uiresources/%2e%2e/a.png",
            "coui://base/textures/uiresources/a.png?x=1","coui://base/textures/uiresources/a\".png",
            "coui://base/textures/uiresources/a.svg","coui://base/textures/uiresources//a.png"}) {
            require(!tutorial_image_url_valid(bad),"unsafe image URL accepted");
            require(s.show(2,0,"Image","Body",1000,bad)==-3,"unsafe image request accepted");
            require(tutorial_body_markup("Body",bad)=="Body","unsafe image became markup");
        }
        require(!tutorial_image_url_valid(image+std::string(256,'a')+".png"),"oversized image accepted");
        auto copied_image=image;
        const auto image_ticket=s.show(2,0,"Image","<plain>",1000,copied_image);copied_image[0]='X';
        require(image_ticket>0 && s.take(0,r) && r.image_url==image,"image URL not copied with request");
        const auto markup=tutorial_body_markup(r.body,r.image_url);
        require(markup.find("data-crml-tutorial-image=\""+image+"\"")!=markup.npos && markup.ends_with("&lt;plain&gt;</span>"),"host image and escaped text composition");
        s.report(image_ticket,CRML_TUTORIAL_DISMISSED);
        crml_tutorial_page page{};page.version=1;page.duration_ms=1000;
        std::strcpy(page.title,"Page");std::strcpy(page.body,"Text\n<escaped>");std::strcpy(page.image_url,image.c_str());
        page.image={CRML_TUTORIAL_IMAGE_BELOW,CRML_TUTORIAL_ALIGN_RIGHT,60,12,2};
        const auto page_ticket=s.present(2,page);page.image.position=0;page.title[0]='X';
        require(page_ticket>0 && s.take(0,r) && r.title=="Page" && r.image.position==CRML_TUTORIAL_IMAGE_BELOW,
            "descriptor not copied into owned request");
        const auto below=tutorial_body_markup(r.body,r.image_url,r.image);
        require(below.find("data-crml-tutorial-layout=\"1,2,60,12,2\"")!=below.npos &&
            below.ends_with("Text<br>&lt;escaped&gt;</span>") && below.find("<img")==below.npos,
            "descriptor metadata or readable text fallback lost");
        s.report(page_ticket,CRML_TUTORIAL_DISMISSED);
        page.kind=CRML_TUTORIAL_PROMPT;page.duration_ms=2000;
        const auto prompt_ticket=s.present(2,page);
        require(prompt_ticket>0 && s.take(CRML_TUTORIAL_PROMPT,r) &&
            r.options==(CRML_TUTORIAL_OPTION_NATIVE_DISMISS|CRML_TUTORIAL_OPTION_PROGRESS),
            "version 1 prompt defaults to native dismissal and progress");
        s.report(prompt_ticket,CRML_TUTORIAL_DISMISSED);
        page.version=CRML_TUTORIAL_PAGE_VERSION_OPTIONS;
        page.reserved=CRML_TUTORIAL_OPTION_PROGRESS;
        const auto quiet_ticket=s.present(2,page);
        require(quiet_ticket>0 && s.take(CRML_TUTORIAL_PROMPT,r) &&
            r.options==CRML_TUTORIAL_OPTION_PROGRESS,"version 2 options copied");
        s.report(quiet_ticket,CRML_TUTORIAL_DISMISSED);
        page.reserved=4;require(s.present(2,page)==-3,"unknown prompt option accepted");
        page.reserved=0;page.kind=CRML_TUTORIAL_HINT;
        page.reserved=CRML_TUTORIAL_OPTION_NATIVE_DISMISS;
        require(s.present(2,page)==-3,"prompt option accepted for passive hint");
        page.reserved=0;page.version=1;page.duration_ms=1000;
        page.version=3;require(s.present(2,page)==-3,"unknown descriptor version accepted");page.version=1;
        page.reserved=1;require(s.present(2,page)==-3,"reserved field accepted");page.reserved=0;
        std::memset(page.title,'x',sizeof(page.title));require(s.present(2,page)==-3,"unterminated title accepted");std::strcpy(page.title,"Page");
        std::memset(page.body,'x',sizeof(page.body));require(s.present(2,page)==-3,"unterminated body accepted");std::strcpy(page.body,"Text");
        std::memset(page.image_url,'x',sizeof(page.image_url));require(s.present(2,page)==-3,"unterminated URL accepted");std::strcpy(page.image_url,image.c_str());
        for(auto invalid: {crml_tutorial_image_layout{2,0,0,0,0},{0,3,0,0,0},{0,0,101,0,0},{0,0,0,33,0},{0,0,0,0,5}}) {
            page.image=invalid;require(s.present(2,page)==-3,"invalid layout accepted");
        }
        require(tutorial_body_markup("text",image)==tutorial_body_markup("text",image,{0,0,100,18,1}),"zero and explicit defaults differ");
        require(tutorial_body_markup("text",{}, {1,2,50,12,2})=="text","image-free page changed text-only output");
        ModTutorials bound;bound.enable(0,true);
        for(uint64_t owner=1;owner<=32;++owner) require(bound.attach(owner),"bounded owner registration");
        require(!bound.attach(33),"owner limit");
        for(uint64_t owner=1;owner<=8;++owner) require(bound.show(owner,0,"Title","Body",1000)>0,"queue slots");
        require(bound.show(9,0,"Title","Body",1000)==-2,"queue bound includes active/native pending tickets");
        ModTutorials texts;texts.enable(0,true);texts.attach(1);
        for(int i=0;i<64;++i) {auto t=texts.show(1,0,"Title",std::to_string(i),1000);require(t>0,"finite distinct text admission");texts.dismiss(1,t);}
        require(texts.show(1,0,"Title","new",1000)==-5,"native localization growth bounded across requests");
        require(texts.show(1,0,"Title","0",1000,image)==-5,"image variations bypassed localization cache bound");
        texts.detach(1);texts.attach(1);
        require(texts.show(1,0,"Title","new",1000)==-5 && texts.show(1,0,"Title","0",1000)>0,"reload cannot bypass text limit, existing text reusable");
        ModTutorials layouts;layouts.enable(0,true);layouts.attach(1);
        for(uint32_t width=1;width<=64;++width) {
            auto t=layouts.show(1,0,"Title","Body",1000,image,{0,0,width,18,1});require(t>0,"layout admission failed");layouts.dismiss(1,t);
        }
        require(layouts.show(1,0,"Title","Body",1000,image,{0,0,65,18,1})==-5,"layout changes bypassed native string budget");
        if(argc==3 && std::string(argv[1])=="--write-layout") {
            std::ofstream html(argv[2]);require(bool(html),"could not write layout fixture");
            html<<"<!doctype html><meta charset=utf-8><style>body{background:#202327;color:#eee;font:20px Arial;margin:20px;display:grid;grid-template-columns:repeat(3,1fr);gap:20px}.case{background:#30343a;padding:20px}p{margin:0}h2{font-size:20px}</style>";
            for(uint32_t position=0;position<2;++position) for(uint32_t alignment=0;alignment<3;++alignment) {
                auto rendered=tutorial_body_markup("Separate text block\n<plain text>",image,{position,alignment,60,12,2});
                html<<"<section class=case data-position="<<position<<" data-alignment="<<alignment<<"><h2>"<<(position?"Below":"Above")<<" / "<<alignment<<"</h2><p cohinline>"<<rendered<<"</p><button>Continue</button></section>";
            }
        }
        std::cout<<"tutorial author queue, ownership, text and cancellation tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
