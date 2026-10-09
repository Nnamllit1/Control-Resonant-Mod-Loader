#include "diagnostics/player_status_lifecycle.h"
#include <iostream>

int main() {
    using namespace crml::player_status_lifecycle::testing;
    if(!callthrough()) {std::cerr<<"Status callback arguments, unwind or passthrough failed\n";return 1;}
    if(!overlapping()) {std::cerr<<"Overlapping Status writes reported as unchanged\n";return 1;}
    if(!reporting()) {std::cerr<<"Status callback reporting failed\n";return 1;}
    if(!installation()) {std::cerr<<"Status callback trampoline installation failed\n";return 1;}
    std::cout<<"Status lifecycle tests passed\n";
    crml::player_status_lifecycle::report(std::cout);
}
