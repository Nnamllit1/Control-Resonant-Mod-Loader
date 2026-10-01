#include "runtime.h"
#include <iostream>
#include <set>
#include <string>
#include <cstring>
struct FakeGame : crml::Gameplay {
    uint32_t capabilities() const noexcept override { return CRML_CAP_INPUT_BUTTONS|CRML_CAP_PLAYER_NOCLIP|
        CRML_CAP_PLAYER_VISIBILITY|CRML_CAP_PHYSICS_DAMPING|CRML_CAP_INPUT_MOTION|CRML_CAP_PLAYER_MOTION|
        CRML_CAP_PLAYER_READ|CRML_CAP_CAMERA_READ|CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|
        CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP|CRML_CAP_UI_PRESENTATION; }
    std::set<uint64_t> owners;
    unsigned calls{}, input_index{};
    unsigned player_reads{};
    unsigned ui_reads{}, tick_index{};
    unsigned media_reads{};
    uint64_t media_generation{0xe123456789abcdefull};
    std::string ui_mode;
    uint32_t ui_screen{CRML_UI_SCREEN_PHOTOSENSITIVITY};
    uint64_t ui_generation{0xf123456789abcdefull};
    float damping{.25f};
    int ui_read(crml_ui_state& out) noexcept override {
        ++calls;
        if(ui_mode.rfind("media-",0)==0 && ui_mode!="media-both") {out={};return -1;}
        if(ui_mode=="ui-present-release-rejected") ui_screen=tick_index<8?1:2;
        if(ui_mode=="ui-changes") {
            constexpr uint32_t screens[]{1,5,8,2,3,4,6,7,9,0};
            ui_screen=screens[(tick_index/8)%10];ui_generation=100+tick_index/8;
        } else if(ui_mode=="ui-generation") {
            ui_generation=100+tick_index/8;
        } else if(ui_mode=="ui-screen-change") {
            constexpr uint32_t screens[]{1,5,8};ui_screen=screens[(tick_index/8)%3];
        } else if(ui_mode=="ui-short") ui_generation=100+tick_index;
        out={CRML_UI_STATE_SIZE,CRML_UI_STATE_VERSION,ui_screen,
            ui_mode=="ui-no-actions"?0u:CRML_UI_ACTION_MASK_CONTINUE,ui_generation,12,0};
        if(ui_mode=="ui-malformed") {
            switch((tick_index/8)%3) {case 0:out.size=31;break;case 1:out.version=2;break;default:out.generation=0;break;}
        }
        if(ui_mode=="ui-transient") return tick_index%9?1:0;
        if(!ui_mode.empty()) return 1;
        return ui_reads++%2?0:1; // Dirty failure is intentional: the host must zero it.
    }
    int ui_activate(uint64_t owner,uint64_t generation,uint32_t action) noexcept override {
        ++calls;
        std::cout<<"UI activate: owner "<<owner<<" generation "<<generation<<" action "<<action
                 <<" screen "<<ui_screen<<" tick "<<tick_index<<'\n';
        if(generation!=ui_generation || action!=CRML_UI_ACTION_CONTINUE) return -3;
        if(ui_mode=="ui-rejected" || ui_mode=="ui-transient") return -1;
        owners.insert(owner);
        return 0;
    }
    int ui_present(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration) noexcept override {
        ++calls;
        std::cout<<"UI present: owner "<<owner<<" generation "<<generation<<" kind "<<kind
                 <<" name "<<name<<" hidden "<<hidden<<" duration "<<duration<<" screen "<<ui_screen<<" tick "<<tick_index<<'\n';
        if(generation!=ui_generation) return -1;
        if(ui_mode=="ui-present-release-rejected" && !hidden && tick_index<10) return -1;
        if(hidden) owners.insert(owner);else owners.erase(owner);
        return 0;
    }
    int media_read(crml_media_state& out) noexcept override {
        ++calls;out={};
        if(ui_mode.rfind("ui-",0)==0) return -1;
        if(ui_mode=="media-generation") media_generation=100+tick_index/16;
        if(ui_mode=="media-short") media_generation=100+tick_index;
        out.size=CRML_MEDIA_STATE_SIZE;out.version=CRML_MEDIA_STATE_VERSION;
        out.flags=CRML_MEDIA_ACTIVE|CRML_MEDIA_SKIPPABLE|CRML_MEDIA_ENGINE_NAME;
        if(ui_mode=="media-mapped") out.flags=CRML_MEDIA_ACTIVE|CRML_MEDIA_SKIPPABLE|CRML_MEDIA_MAPPED_NAME;
        if(ui_mode=="media-unnamed") out.flags=CRML_MEDIA_ACTIVE|CRML_MEDIA_SKIPPABLE;
        if(ui_mode=="media-inactive") out.flags&=~CRML_MEDIA_ACTIVE;
        if(ui_mode=="media-unskippable") out.flags&=~CRML_MEDIA_SKIPPABLE;
        out.elapsed_ms=ui_mode=="media-young"?2000:ui_mode=="media-clock"?tick_index*125:2501;
        out.generation=media_generation;
        std::string path="textures\\videos\\uiresources\\splash\\boot.tex";
        if(ui_mode=="media-slashes") path="textures/videos/uiresources/splash/boot.tex";
        if(ui_mode=="media-unknown") path="textures/videos/uiresources/story/cutscene.tex";
        if(ui_mode=="media-suffix") path+=".extra";
        out.name_length=static_cast<uint32_t>(path.size());
        std::memcpy(out.name,path.c_str(),path.size()+1);
        if(ui_mode=="media-nul") out.name[8]=0;
        if(ui_mode=="media-unterminated") out.name[out.name_length]='x';
        if(ui_mode=="media-length") out.name_length=CRML_MEDIA_NAME_CAPACITY;
        if(ui_mode=="media-malformed") {
            switch((tick_index/8)%3) {case 0:out.size=287;break;case 1:out.version=2;break;default:out.generation=0;break;}
        }
        if(ui_mode=="media-transient") return tick_index%6?1:-1;
        if(!ui_mode.empty()) return 1;
        return media_reads++%2?-1:1;
    }
    int media_skip(uint64_t owner,uint64_t generation) noexcept override {
        ++calls;
        std::cout<<"Media skip: owner "<<owner<<" generation "<<generation<<" tick "<<tick_index<<'\n';
        if(generation!=media_generation) return -1;
        if(ui_mode=="media-rejected" || ui_mode=="media-transient") return -1;
        owners.insert(owner);return 0;
    }
    int player_read(crml_player_state& out) noexcept override {
        ++calls;out={1,12,0x123456789abcdefull,{1,2,3},0};
        return player_reads++%2?0:1; // Failed provider intentionally leaves data: host must clear it.
    }
    int camera_read(crml_camera_state& out) noexcept override {
        ++calls;out={1,10,0x23456789abcdef0ull,1,CRML_CAMERA_STATE_POSE|CRML_CAMERA_STATE_LENS,
            {4,5,6},{1,0,0,0,1,0,0,0,1},1.25f,1.75f};return 1;
    }
    int physics_read(uint64_t owner,uint64_t target,crml_physics_state& out) noexcept override {
        ++calls;out={1,5,CRML_PHYSICS_STATE_DAMPING|CRML_PHYSICS_STATE_SPEED,0,damping,.5f,3,4};
        return owners.count(owner) && target==0x123456789abcdefull?1:-3;
    }
    unsigned motion_input_index{};
    uint32_t input_motion() noexcept override {constexpr uint32_t buttons[]{1,2,130,0};return buttons[(motion_input_index++)%4];}
    int motion_camera(float (&right)[2]) noexcept override {right[0]=0;right[1]=-1;return 1;}
    int motion_set(uint64_t owner,bool enabled,float x,float y,float z) noexcept override {
        ++calls;if(enabled) owners.insert(owner);else owners.erase(owner);
        std::cout<<"Motion request: "<<enabled<<" velocity "<<x<<','<<y<<','<<z<<'\n';return enabled?1:0;
    }
    uint32_t input_buttons() noexcept override { constexpr uint32_t buttons[]{0,1,1,2}; return buttons[(input_index++)%4]; }
    int visibility_set(uint64_t owner,bool hidden) noexcept override {
        if(hidden) owners.insert(owner); else owners.erase(owner);
        ++calls; std::cout<<"Visibility request: "<<hidden<<'\n'; return hidden?1:0;
    }
    int noclip_poll(uint64_t owner,float) noexcept override { owners.insert(owner); ++calls; return 0; }
    int visibility_poll(uint64_t owner) noexcept override { owners.insert(owner); ++calls; return 1; }
    int physics_select(uint64_t owner) noexcept override {owners.insert(owner);++calls;return 0;}
    int physics_select_near(uint64_t owner,float x,float y,float z,float radius) noexcept override {
        std::cout<<"Physics query: "<<x<<','<<y<<','<<z<<" radius "<<radius<<'\n';
        return physics_select(owner);
    }
    uint64_t physics_target(uint64_t owner) noexcept override {++calls;return owners.count(owner)?0x123456789abcdefull:0;}
    int physics_apply(uint64_t owner,uint64_t handle,float value,uint32_t duration) noexcept override {
        ++calls;
        if(!owners.count(owner) || handle!=0x123456789abcdefull) return -3;
        damping=value;
        std::cout<<"Physics apply: owner-scoped token, damping "<<value<<", duration "<<duration<<'\n';return 0;
    }
    int physics_status(uint64_t owner) noexcept override {++calls;return owners.count(owner)?3:0;}
    int physics_restore(uint64_t owner) noexcept override {++calls;owners.erase(owner);return 0;}
    void release(uint64_t owner) noexcept override { owners.erase(owner); }
};
struct FakeInput : crml::Input {
    unsigned calls{};
    bool available() noexcept override { return true; }
    uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& keys) noexcept override {
        // A deterministic held F10; other keys are released. The host must mask
        // unknown/high bits even if a provider returns them.
        uint32_t result=0xffff0000u;
        const auto frame=calls++%4;
        for(size_t i=0;i<keys.size();++i)
            if(keys[i]==0x79 || (keys[i]==0x24 && frame==1) || (keys[i]==0x23 && frame==3)) result|=1u<<i;
        return result;
    }
};
int main(int argc,char** argv) {
    if(argc!=2 && argc!=3) return 2;
    FakeGame game;
    FakeInput input;
    size_t failures{};
    {
        crml::Runtime runtime([](const auto& s){std::cout<<s<<'\n';},&game,&input);
        runtime.load(argv[1],[](uint32_t requested){std::cout<<"Prepared capabilities: "<<requested<<'\n';});
        int ticks=argc==3?4:1;
        float delta=.1f;
        if(argc==3 && std::string(argv[2]).rfind("ui-",0)==0) {
            game.ui_mode=argv[2];delta=.25f;ticks=64;
            if(game.ui_mode.rfind("ui-screen-",0)==0 && game.ui_mode!="ui-screen-change")
                game.ui_screen=static_cast<uint32_t>(std::stoul(game.ui_mode.substr(10)));
            if(game.ui_mode=="ui-changes") ticks=80;
            if(game.ui_mode=="ui-generation" || game.ui_mode=="ui-screen-change") ticks=24;
            if(game.ui_mode=="ui-transient") ticks=128;
        }
        if(argc==3 && std::string(argv[2]).rfind("media-",0)==0) {
            game.ui_mode=argv[2];delta=.125f;ticks=64;
            if(game.ui_mode=="media-generation") ticks=48;
        }
        for(int i=0;i<ticks;++i) {game.tick_index=static_cast<unsigned>(i);runtime.tick(delta);}
        runtime.shutdown();
        failures=runtime.failures();
        if(!game.owners.empty()) return 3;
    }
    if(!game.owners.empty()) return 4;
    std::cout<<"Gameplay calls: "<<game.calls<<"; failures: "<<failures<<"; owners: 0\n";
    return 0;
}
