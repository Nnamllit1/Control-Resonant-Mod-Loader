#include "action_rules.h"
#include <iostream>
#include <stdexcept>
namespace {
bool resolve(crml::action_rules::Identity& out) noexcept {out={123,456,7};return true;}
uint64_t clock_now() noexcept {return 1000;}
}
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    crml::action_rules::Service service(&resolve,&clock_now);
    service.available(true);
    crml::Runtime runtime([](const std::string& s){std::cout<<s<<'\n';},&service);
    runtime.load(argv[1]);
    const bool trap=std::string_view(argv[2])=="trap";
    const auto active=service.allowed({123,456,7},1000);
    if((trap && (runtime.failures()!=1 || active)) ||
       (!trap && (runtime.failures() || active!=0x10033))) return 1;
    runtime.shutdown();
    if(service.allowed({123,456,7},1000)) return 1;
    std::cout<<"Owner request and automatic cleanup passed\n";
}
