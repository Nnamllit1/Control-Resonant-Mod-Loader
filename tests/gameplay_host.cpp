#include "runtime.h"
#include <iostream>
#include <set>
struct FakeGame : crml::Gameplay {
    std::set<uint64_t> owners;
    unsigned calls{}, input_index{};
    uint32_t input_buttons() noexcept override { constexpr uint32_t buttons[]{0,1,1,2}; return buttons[(input_index++)%4]; }
    int visibility_set(uint64_t owner,bool hidden) noexcept override {
        if(hidden) owners.insert(owner); else owners.erase(owner);
        ++calls; std::cout<<"Visibility request: "<<hidden<<'\n'; return hidden?1:0;
    }
    int noclip_poll(uint64_t owner,float) noexcept override { owners.insert(owner); ++calls; return 0; }
    int visibility_poll(uint64_t owner) noexcept override { owners.insert(owner); ++calls; return 1; }
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
