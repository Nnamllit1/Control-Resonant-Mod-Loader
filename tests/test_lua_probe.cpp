#include "lua_probe.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace crml::probe::lua;
namespace {
constexpr uintptr_t image=0x10000,site=image+0x1a0aadf;
std::array<unsigned char,256> vm{},context{};
std::array<unsigned char,4096> global{},stack{},moved{};
std::array<unsigned char,0x58600> world{};
std::array<uint64_t,2> generations{};
std::array<unsigned char,40> ci{};
int calls{},loads{},protects{},restores{},mode{};
bool deliberate{};
constexpr uint64_t owner_id=0x200000001;
Owner owner() {return {reinterpret_cast<uintptr_t>(world.data()),owner_id};}
template<class T> void put(void* p,size_t offset,T value) {std::memcpy(static_cast<unsigned char*>(p)+offset,&value,sizeof(value));}
template<class T> T get(void* p,size_t offset) {T v;std::memcpy(&v,static_cast<unsigned char*>(p)+offset,sizeof(v));return v;}
void require(bool value,const char* text) {if(!value) {std::cerr<<text<<'\n';std::exit(1);}}
void push(int tag,double value=0) {
    auto top=get<uintptr_t>(vm.data(),8);
    put(reinterpret_cast<void*>(top),0,value);put(reinterpret_cast<void*>(top),16,tag);
    put(vm.data(),8,top+24);
}
int load(void* l,const char* label,const char* bytes,size_t size,int env) {
    require(l==vm.data() && env==0 && size>2 && bytes[0]==6 && bytes[1]==3,"loader ABI/bytecode");
    ++loads;deliberate=std::strstr(label,"error")!=nullptr;
    after_call(vm.data(),site,1,0,1,0); // Reentrancy must not start another run.
    if(mode==1) {push(5);return 1;}
    if(mode==2) throw 4;
    push(7);return 0;
}
int call(void* l,int args,int results,int err) {
    require(l==vm.data() && args==(calls==4?1:0) && results==1 && err==0,"protected call ABI");
    ++calls;
    put(vm.data(),8,get<uintptr_t>(vm.data(),8)-(args+1)*24);
    push(deliberate?5:3,calls>=4?127:42);
    if(mode==3) {
        const auto old=get<uintptr_t>(vm.data(),0x30);
        const auto top=get<uintptr_t>(vm.data(),8)-old;
        const auto base=get<uintptr_t>(vm.data(),0x10)-old;
        std::memcpy(moved.data(),reinterpret_cast<void*>(old),stack.size());
        put(vm.data(),0x30,reinterpret_cast<uintptr_t>(moved.data()));
        put(vm.data(),8,reinterpret_cast<uintptr_t>(moved.data())+top);
        put(vm.data(),0x10,reinterpret_cast<uintptr_t>(moved.data())+base);
        put(vm.data(),0x28,reinterpret_cast<uintptr_t>(moved.data()+moved.size()-24));
        put(ci.data(),0,reinterpret_cast<uintptr_t>(moved.data())+base);
        mode=0;
    }
    if(mode==4) put(stack.data(),24,uint64_t{123}); // Corrupt a pre-existing value.
    return deliberate?2:0;
}
int protect(void* l,Body body,void* user,ptrdiff_t top,ptrdiff_t err) {
    require(l==vm.data() && top==48 && err==0,"barrier receives saved stack offset");
    ++protects;
    try {body(l,user);return 0;} catch(int status) {
        put(vm.data(),8,get<uintptr_t>(vm.data(),0x30)+top);push(5);return status;
    }
}
void settop(void* l,int n) {
    require(l==vm.data() && n==1,"restore saved API stack index");++restores;
    put(l,8,get<uintptr_t>(l,0x10)+n*24);
}
void push_entity(void* l,uint64_t id,int tag) {
    require(l==vm.data() && id==owner_id && tag==1,"owner push ABI");
    auto top=get<uintptr_t>(l,8);push(2);
    put(reinterpret_cast<void*>(top),0,id);put(reinterpret_cast<void*>(top),8,tag);
}
void reset(int m=0) {
    vm={};context={};global={};stack={};moved={};ci={};world={};generations={0,2};
    calls=loads=protects=restores=0;mode=m;deliberate=false;
    auto ptr=[](auto& a) {return reinterpret_cast<uintptr_t>(a.data());};
    put(vm.data(),8,ptr(stack)+48);put(vm.data(),0x10,ptr(stack)+24);
    put(vm.data(),0x18,ptr(global));put(vm.data(),0x20,ptr(ci));
    put(vm.data(),0x28,ptr(stack)+stack.size()-24);put(vm.data(),0x30,ptr(stack));
    put(vm.data(),0x40,ptr(ci));put(vm.data(),0x58,uintptr_t{0x1234});
    put(vm.data(),0x78,ptr(context));put(context.data(),0,ptr(world));
    put(world.data(),0x584e8,ptr(generations));put(world.data(),0x58510,uint64_t{2});
    put(ci.data(),0,ptr(stack)+24);put(stack.data(),24+16,uint32_t{7});
    configure({&load,&protect,&settop,&call,&push_entity},image);
}
std::string output() {std::ostringstream out;write(out);return out.str();}
void invoke() {after_call(vm.data(),site,1,0,1,0,owner());}
}
int main() {
    reset();
    after_call(vm.data(),site+1,1,0,1,0);after_call(vm.data(),site,0,0,1,0);
    after_call(vm.data(),site,1,1,1,0);after_call(vm.data(),site,1,0,0,0);
    after_call(vm.data(),site,1,0,1,2);
    require(loads==0,"only the reviewed successful call site may run");
    invoke();require(loads==5 && calls==5 && protects==5 && restores==5,"all five phases");
    require(output().find("\"passed\":true")!=std::string::npos,"successful report");
    require(output().find("\"value\":127")!=std::string::npos,"capability result");
    invoke();require(loads==5,"once per process");
    reset(1);invoke();require(loads==1 && calls==0 && restores==1,"load error cleaned, remaining phases stopped");
    require(output().find("\"passed\":false")!=std::string::npos,"load failure report");
    reset(2);invoke();require(loads==1 && restores==1,"allocation exception contained and cleaned");
    require(output().find("\"protect\":4")!=std::string::npos,"barrier failure report");
    reset(3);invoke();require(loads==5 && restores==5,"stack relocation handled by offsets");
    require(output().find("\"passed\":true")!=std::string::npos,"relocation report");
    reset(4);invoke();require(loads==1,"pre-existing payload corruption stops test");
    require(output().find("\"restored\":false")!=std::string::npos,"corruption reported");
    reset();put(global.data(),0xd28,uintptr_t{1});invoke();require(loads==0,"debugger suspension rejected");
    reset();put(vm.data(),3,uint8_t{1});invoke();require(loads==0,"suspended VM rejected");
    reset();put(vm.data(),0x78,uintptr_t{});invoke();require(loads==0,"missing world context rejected");
    reset();after_call(reinterpret_cast<void*>(1),site,1,0,1,0);require(loads==0,"unreadable VM rejected");
    reset();generations[1]=3;invoke();require(loads==0,"replaced owner rejected");
    reset();auto wrong=owner();wrong.world=0;after_call(vm.data(),site,1,0,1,0,wrong);require(loads==0,"foreign world rejected");
    reset();push(7);push_entity(vm.data(),owner_id,1);
    const auto saved=stack;const auto captured=before_call(vm.data(),site,1,0,1);
    require(captured.world==owner().world && captured.entity==owner_id && stack==saved,"read-only incoming owner capture");
    require(before_call(vm.data(),site+1,1,0,1).entity==0,"no capture from other callers");
    put(stack.data(),72+8,uint32_t{4});require(before_call(vm.data(),site,1,0,1).entity==0,"wrong userdata kind rejected");
    put(stack.data(),72+8,uint32_t{1});generations[1]=3;
    require(before_call(vm.data(),site,1,0,1).entity==0,"stale incoming owner rejected");
    reset();stop();invoke();require(loads==0,"stopped probe stays passive");
    std::cout<<"Lua probe checks passed\n";
}
