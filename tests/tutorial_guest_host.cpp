#include "runtime.h"
#include "mod_tutorials.h"
#include "action_bindings.h"
#include <iostream>
#include <stdexcept>
struct Keys : crml::Input {
    uint32_t flags=CRML_INPUT_AVAILABLE|CRML_INPUT_FOCUSED|CRML_INPUT_FRESH|CRML_INPUT_CONTEXT_KNOWN;
    uint16_t held{};
    bool available() noexcept override {return true;}
    uint32_t state_flags() noexcept override {return flags;}
    uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept override {
        uint32_t result{};for(size_t i=0;i<keys.size();++i)if(held && keys[i]==held)result|=1u<<i;return result;
    }
};
void require(bool b,const char* text) {if(!b)throw std::runtime_error(text);}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2,"mods path required");Keys keys;crml::ModTutorials service;
        service.enable(CRML_TUTORIAL_PANEL,true);
        crml::Runtime runtime([](const std::string& text){std::cout<<text<<'\n';},nullptr,&keys,{},nullptr,nullptr,nullptr,{},&service);
        runtime.load(argv[1]);require(runtime.active() && !runtime.failures(),"real tutorial guest loaded");
        runtime.tick(.01f);keys.held=crml::action_key("F10");runtime.tick(.01f);
        crml::ModTutorials::Request request;require(service.take(CRML_TUTORIAL_PANEL,request),"panel requested by F10");
        require(crml::tutorial_image_url_valid(request.image_url),"compiled guest did not submit its image through the Wasm import");
        require(request.image.position==CRML_TUTORIAL_IMAGE_BELOW && request.image.width_percent==80 && request.image.gap_vh==2,
            "compiled guest descriptor layout not copied");
        service.report(request.ticket,CRML_TUTORIAL_PRESENTED);keys.held=0;runtime.tick(.01f);
        const auto fresh=keys.flags;
        keys.flags&=~CRML_INPUT_FOCUSED;keys.held=crml::action_key("End");runtime.tick(.01f);
        require(!service.cancelled(request.ticket),"unfocused End ignored");
        keys.flags=fresh;runtime.tick(.01f);
        require(service.cancelled(request.ticket),"held End cancels on first fresh sample");
        service.report(request.ticket,CRML_TUTORIAL_CANCELLED);runtime.tick(.01f);
        keys.flags&=~CRML_INPUT_FRESH;runtime.tick(.01f);
        keys.flags=fresh;keys.held=crml::action_key("F10");runtime.tick(.01f);
        require(!service.take(CRML_TUTORIAL_PANEL,request),"held open key on rearming does not open panel");
        keys.held=0;runtime.tick(.01f);keys.held=crml::action_key("F10");runtime.tick(.01f);
        require(service.take(CRML_TUTORIAL_PANEL,request),"fresh F10 edge opens another panel");
        service.report(request.ticket,CRML_TUTORIAL_PRESENTED);keys.held=crml::action_key("End");runtime.tick(.01f);
        require(service.cancelled(request.ticket),"ordinary End press cancels");
        service.report(request.ticket,CRML_TUTORIAL_CANCELLED);runtime.shutdown();
        require(!runtime.failures(),"no guest traps");std::cout<<"Tutorial guest cancellation passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
