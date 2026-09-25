#include "runtime.h"
#include <iostream>
#include <set>
struct FakeGame : crml::Gameplay {
    std::set<uint64_t> owners;
    unsigned calls{};
    int noclip_poll(uint64_t owner,float) noexcept override { owners.insert(owner); ++calls; return 0; }
    void release(uint64_t owner) noexcept override { owners.erase(owner); }
};
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    FakeGame game;
    size_t failures{};
    {
        crml::Runtime runtime([](const auto& s){std::cout<<s<<'\n';},&game);
        runtime.load(argv[1]);
        runtime.tick(.1f);
        runtime.shutdown();
        failures=runtime.failures();
        if(!game.owners.empty()) return 3;
    }
    if(!game.owners.empty()) return 4;
    std::cout<<"Gameplay calls: "<<game.calls<<"; failures: "<<failures<<"; owners: 0\n";
    return 0;
}
