#include "runtime.h"
#include <iostream>
#include <set>
struct FakeGame : crml::Gameplay {
    std::set<uint64_t> owners;
    unsigned calls{}, input_index{};
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
    uint64_t physics_target(uint64_t owner) noexcept override {++calls;return owners.count(owner)?0x123456789abcdefull:0;}
    int physics_apply(uint64_t owner,uint64_t handle,float value,uint32_t duration) noexcept override {
        ++calls;
        if(!owners.count(owner) || handle!=0x123456789abcdefull || value!=8 || duration!=5000) return -3;
        std::cout<<"Physics apply: owner-scoped token, damping 8, duration 5000\n";return 0;
    }
    int physics_status(uint64_t owner) noexcept override {++calls;return owners.count(owner)?3:0;}
    int physics_restore(uint64_t owner) noexcept override {++calls;owners.erase(owner);return 0;}
    void release(uint64_t owner) noexcept override { owners.erase(owner); }
};
int main(int argc,char** argv) {
    if(argc!=2 && argc!=3) return 2;
    FakeGame game;
    size_t failures{};
    {
        crml::Runtime runtime([](const auto& s){std::cout<<s<<'\n';},&game);
        runtime.load(argv[1]);
        for(int i=0;i<(argc==3?4:1);++i) runtime.tick(.1f);
        runtime.shutdown();
        failures=runtime.failures();
        if(!game.owners.empty()) return 3;
    }
    if(!game.owners.empty()) return 4;
    std::cout<<"Gameplay calls: "<<game.calls<<"; failures: "<<failures<<"; owners: 0\n";
    return 0;
}
