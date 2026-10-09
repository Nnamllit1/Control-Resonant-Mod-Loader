#include "diagnostics/structural_lifecycle.h"
#include <iostream>

int main() {
    using namespace crml::structural_lifecycle::testing;
    if(!callthrough()) {std::cerr<<"Argument, nested-call, unwind or stop contract failed\n";return 1;}
    if(!concurrent()) {std::cerr<<"Overlapping native work was reported as quiet\n";return 1;}
    if(!reporting()) {std::cerr<<"Lifecycle report contract failed\n";return 1;}
    std::cout<<"Structural lifecycle observer tests passed\n";
    // Also provide an actual producer record for offline analyzer integration.
    crml::structural_lifecycle::report(std::cout);
}
