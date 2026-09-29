#include "lua_references.h"
#include <Windows.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
using namespace crml::probe::lua::references;
namespace {
constexpr uintptr_t image=0x10000,registration=image+0x1a15722,removal=image+0x1a08f15,owner_cleanup=image+0x1a09648;
std::array<unsigned char,128> vm{},stack{},closure{},source{};
std::array<unsigned char,256> proto{};
constexpr uintptr_t global=0xabcdefabcd;
int retain_calls{},release_calls{},reference=71;
bool fail_release{},free_vm{};
void require(bool ok,const char* message) {if(!ok) {std::cerr<<message<<'\n';std::exit(1);}}
template<class T> void put(void* p,size_t n,T value) {std::memcpy(static_cast<unsigned char*>(p)+n,&value,sizeof(value));}
int original_retain(void*,int) {++retain_calls;return reference;}
void original_release(void* p,int) {
    ++release_calls;
    if(fail_release) throw 7;
    if(free_vm) require(VirtualFree(p,0,MEM_RELEASE)!=0,"free original VM");
}
std::string report() {std::ostringstream out;write(out);return out.str();}
void field(const char* key,int value) {
    require(report().find(std::string("\"")+key+"\":"+std::to_string(value)+",")!=std::string::npos,key);
}
void reset() {
    vm={};stack={};closure={};source={};proto={};retain_calls=release_calls=0;reference=71;fail_release=free_vm=false;
    put(vm.data(),8,reinterpret_cast<uintptr_t>(stack.data()+24));put(vm.data(),0x10,reinterpret_cast<uintptr_t>(stack.data()));
    put(vm.data(),0x18,global);put(stack.data(),0,reinterpret_cast<uintptr_t>(closure.data()));put(stack.data(),16,uint32_t{7});
    put(closure.data(),0,uint8_t{7});put(closure.data(),0x18,reinterpret_cast<uintptr_t>(proto.data()));
    put(proto.data(),0x58,reinterpret_cast<uintptr_t>(source.data()));
    constexpr char label[]="=crml_persistent_events";
    put(source.data(),0,uint8_t{5});put(source.data(),0x14,uint32_t{sizeof(label)-1});
    std::memcpy(source.data()+0x18,label,sizeof(label)-1);
    testing::configure(original_retain,original_release,image);
}
}
int main() {
    reset();const auto saved=vm,values=stack;
    require(testing::retain(vm.data(),1,registration)==71 && vm==saved && stack==values,"retain forwards and observes without VM writes");
    field("retained",1);field("active",1);
    testing::release(vm.data(),71,removal);field("released",1);field("explicit_removals",1);field("active",0);
    testing::retain(vm.data(),-1,registration);testing::release(vm.data(),71,owner_cleanup);
    field("retained",2);field("owner_removals",1);field("lost",0);
    require(retain_calls==2 && release_calls==2,"all original calls forwarded exactly once");
    require(report().find(std::to_string(global))==std::string::npos,"no raw global addresses logged");
    reset();testing::retain(vm.data(),1,registration+1);field("retained",0);
    put(source.data(),0x18,char{'x'});testing::retain(vm.data(),1,registration);field("retained",0);
    reset();testing::retain(vm.data(),2,registration);testing::retain(vm.data(),-2,registration);testing::retain(vm.data(),-10000,registration);field("retained",0);
    reset();put(closure.data(),3,uint8_t{1});testing::retain(vm.data(),1,registration);field("retained",0);
    reset();testing::retain(vm.data(),1,registration);testing::retain(vm.data(),1,registration);field("lost",1);field("active",1);
    reset();for(int i=0;i<17;++i) {reference=i+1;testing::retain(vm.data(),1,registration);}field("retained",16);field("lost",1);
    reset();testing::retain(vm.data(),1,registration);close(global);field("active",0);field("vm_reclaimed",1);
    testing::retain(vm.data(),1,registration);testing::release(vm.data(),71,image+0x1a160f1);field("error_removals",1);
    reset();testing::retain(vm.data(),1,registration);put(vm.data(),0x18,uintptr_t{global+1});testing::release(vm.data(),71,removal);field("active",1);field("released",0);
    reset();testing::retain(vm.data(),1,registration);fail_release=true;
    try {testing::release(vm.data(),71,removal);} catch(int) {}
    field("pending",1);field("released",0);field("active",0);
    reset();testing::retain(reinterpret_cast<void*>(1),1,registration);field("read_failures",1);
    reset();auto* allocated=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    require(allocated!=nullptr,"allocate fixture VM");std::memcpy(allocated,vm.data(),vm.size());
    testing::retain(allocated,1,registration);free_vm=true;testing::release(allocated,71,removal);
    field("released",1);field("read_failures",0);
    std::cout<<"Lua listener reference checks passed\n";
}
