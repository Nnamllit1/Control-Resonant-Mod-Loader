#pragma once
#include "runtime.h"

namespace crml {
// Services keep independent owners. Releasing a mod visits both providers,
// including unavailable services that may still have restoration pending.
class GameplayRouter final : public Gameplay {
public:
    GameplayRouter(Gameplay& player,Gameplay& physics,Gameplay* camera=nullptr,Gameplay* ui=nullptr,Gameplay* media=nullptr):player_(player),physics_(physics),camera_(camera),ui_(ui),media_(media){}
    uint32_t capabilities() const noexcept override {return player_.capabilities()|physics_.capabilities()|(camera_?camera_->capabilities():0u)|(ui_?ui_->capabilities():0u)|(media_?media_->capabilities():0u);}
    int media_read(crml_media_state& out) noexcept override {if(media_) return media_->media_read(out);out={};return -1;}
    int media_skip(uint64_t o,uint64_t g) noexcept override {return media_?media_->media_skip(o,g):-1;}
    int ui_read(crml_ui_state& out) noexcept override {if(ui_) return ui_->ui_read(out);out={};return -1;}
    int ui_activate(uint64_t o,uint64_t g,uint32_t a) noexcept override {return ui_?ui_->ui_activate(o,g,a):-1;}
    int ui_present(uint64_t o,uint64_t g,uint32_t k,std::string_view n,bool h,uint32_t d) noexcept override {return ui_?ui_->ui_present(o,g,k,n,h,d):-1;}
    int player_read(crml_player_state& out) noexcept override {return player_.player_read(out);}
    int camera_read(crml_camera_state& out) noexcept override {if(camera_) return camera_->camera_read(out);out={};return -1;}
    int physics_read(uint64_t o,uint64_t t,crml_physics_state& out) noexcept override {return physics_.physics_read(o,t,out);}
    int noclip_poll(uint64_t o,float v) noexcept override {return player_.noclip_poll(o,v);}
    uint32_t input_buttons() noexcept override {return player_.input_buttons()|physics_.input_buttons();}
    uint32_t input_motion() noexcept override {return player_.input_motion();}
    int motion_camera(float (&v)[2]) noexcept override {return player_.motion_camera(v);}
    int motion_set(uint64_t o,bool e,float x,float y,float z) noexcept override {return player_.motion_set(o,e,x,y,z);}
    int visibility_set(uint64_t o,bool h) noexcept override {return player_.visibility_set(o,h);}
    int visibility_poll(uint64_t o) noexcept override {return player_.visibility_poll(o);}
    int physics_select(uint64_t o) noexcept override {return physics_.physics_select(o);}
    int physics_select_near(uint64_t o,float x,float y,float z,float r) noexcept override {return physics_.physics_select_near(o,x,y,z,r);}
    uint64_t physics_target(uint64_t o) noexcept override {return physics_.physics_target(o);}
    int physics_apply(uint64_t o,uint64_t t,float v,uint32_t d) noexcept override {return physics_.physics_apply(o,t,v,d);}
    int physics_status(uint64_t o) noexcept override {return physics_.physics_status(o);}
    int physics_restore(uint64_t o) noexcept override {return physics_.physics_restore(o);}
    void release(uint64_t o) noexcept override {player_.release(o);physics_.release(o);if(camera_) camera_->release(o);if(ui_) ui_->release(o);if(media_) media_->release(o);}
private:
    Gameplay& player_;
    Gameplay& physics_;
    Gameplay* camera_{};
    Gameplay* ui_{};
    Gameplay* media_{};
};
}
