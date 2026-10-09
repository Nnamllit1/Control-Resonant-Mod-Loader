#include <iostream>
namespace crml::tutorial_native {bool test_contract();}
int main() {
    if(!crml::tutorial_native::test_contract()) {std::cerr<<"Native panel ownership contract failed\n";return 1;}
    std::cout<<"Native panel insertion ABI, borrowed query arbitration, sync acknowledgement and retirement checks passed\n";
}
