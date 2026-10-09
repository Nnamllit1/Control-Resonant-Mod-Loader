#include "tutorial_payload.h"
#include <Windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
using namespace crml::tutorial;
unsigned assigned{},destroyed{},built{},freed{};
bool fault_assign{},fault_build{};
std::string title,body;
void require(bool b,const char* why) {if(!b) throw std::runtime_error(why);}
uint64_t word(const void* p) {uint64_t v{};std::memcpy(&v,p,8);return v;}
void* assign(void* p,const NativeStringView* view) {
    require(word(p)==0x04000004,"source string uses native inline header");
    if(fault_assign) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    (++assigned%2?title:body).assign(view->data,view->size);return p;
}
void destroy_string(void*) {++destroyed;}
PageVector* build(PageVector* out,const PageSource* pages) {
    ++built;
    require(pages->count==1 && pages->data,"one native source page");
    const auto* s=static_cast<const unsigned char*>(pages->data);
    require(!word(s+8) && !word(s+0x60) && !word(s+0x68),"no media token or conditional allocations");
    require(word(s+0x74)==0x02000002 && s[0x70]==1,"media string header and heading flag");
    require(!out->data && !out->count && !out->capacity,"builder gets fresh native vector");
    if(fault_build) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    out->data=reinterpret_cast<void*>(0x12340000);out->count=out->capacity=1;return out;
}
void destroy_pages(PageVector* pages) {++freed;*pages={};}
}
int main() {
    try {
        PayloadNative native{1,assign,destroy_string,build,destroy_pages};PageVector pages{};
        require(native.make("Heading","Plain message",pages),"build complete native page");
        require(title=="Heading" && body=="Plain message" && assigned==2 && destroyed==3 && built==1,"source ownership is released after deep copy");
        require(!native.make("another","message",pages) && built==1,"reject replacement of owned vector");
        require(native.destroy(pages) && freed==1 && !pages.data,"matching allocator cleanup resets ownership");
        require(!native.make("Heading","",pages),"empty body rejected before allocation");
        fault_assign=true;
        require(!native.make("Heading","Message",pages) && destroyed==3 && built==1,"assignment AV does not speculate destructor ownership");
        fault_assign=false;fault_build=true;
        require(!native.make("Heading","Message",pages) && destroyed==3 && built==2 && freed==1,"builder AV quarantines partial ownership");
        PayloadNative unbound{};require(!PayloadNative::bind(0,unbound),"unsupported binary refused");
        std::cout<<"Native tutorial payload ABI and fault ownership checks passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
