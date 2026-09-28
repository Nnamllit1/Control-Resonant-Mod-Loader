#include "lua_lifetime.h"
#include <Windows.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

using namespace crml::probe::lua::lifetime;
namespace {
std::array<unsigned char,128> vm{},script{};
uintptr_t global=0x123456789abcdef0;
unsigned closed{},cleaned{};
bool release_vm{},release_script{};
void require(bool ok,const char* message) {
    if(!ok) {std::cerr<<message<<'\n';std::exit(1);}
}
template<class T> void put(void* p,size_t offset,T value) {
    std::memcpy(static_cast<unsigned char*>(p)+offset,&value,sizeof(value));
}
void close_original(void* p) {
    ++closed;
    if(release_vm) require(VirtualFree(p,0,MEM_RELEASE)!=0,"free VM in original close");
}
void cleanup_original(void* p,void* b,void* c,void* d,void* e,void* f,void* g,uint64_t owner) {
    ++cleaned;
    require(b==reinterpret_cast<void*>(2) && c==reinterpret_cast<void*>(3) && d==reinterpret_cast<void*>(4) &&
            e==reinterpret_cast<void*>(5) && f==reinterpret_cast<void*>(6) && g==reinterpret_cast<void*>(7) && owner==9,
            "all eight cleanup parameters forwarded intact");
    if(release_script) require(VirtualFree(p,0,MEM_RELEASE)!=0,"free script in original cleanup");
}
void cleanup(void* state) {testing::cleanup(state,(void*)2,(void*)3,(void*)4,(void*)5,(void*)6,(void*)7,9);}
void reset() {
    closed=cleaned=0;release_vm=release_script=false;vm={};script={};
    put(vm.data(),0x18,global);put(script.data(),8,vm.data());
    testing::configure(close_original,cleanup_original);
}
std::string drain() {std::ostringstream out;write(out);return out.str();}
size_t occurrences(const std::string& s,const char* what) {
    size_t n=0,at=0;while((at=s.find(what,at))!=std::string::npos) {++n;at+=std::strlen(what);}return n;
}
}
int main() {
    reset();const auto before_vm=vm;const auto before_script=script;
    observe(vm.data(),101,9);observe(vm.data(),101,10);
    auto log=drain();
    require(vm==before_vm && script==before_script,"observation is read-only");
    require(occurrences(log,"\"event\":\"vm_observed\"")==1 && occurrences(log,"owner_observed")==1,"stable owner and VM across calls");
    require(log.find("123456789abcdef0")==std::string::npos && log.find(std::to_string(global))==std::string::npos,"raw VM identity not serialized");
    require(log.find("\"incomplete\":false")!=std::string::npos,"complete initial trace");
    cleanup(script.data());log=drain();
    require(cleaned==1 && log.find("owner_cleanup_begin")<log.find("owner_cleanup_end"),"cleanup forwards once with paired events");
    observe(vm.data(),101,9);log=drain();
    require(log.find("\"owner_epoch\":4")!=std::string::npos,"cleaned owner gets new epoch even when ID reused");
    observe(vm.data(),102,9);log=drain();
    require(log.find("world_changed")<log.find("world_observed") && log.find("\"world_epoch\":5")!=std::string::npos,"world replacement retires ownership");
    testing::close(vm.data());observe(vm.data(),102,9);log=drain();
    require(closed==1 && log.find("vm_close_begin")<log.find("vm_close_end"),"close forwards once with paired events");
    require(log.find("\"vm_epoch\":7")!=std::string::npos,"same VM address after close creates fresh epoch");
    stop();drain();observe(reinterpret_cast<void*>(1),1,9);cleanup(reinterpret_cast<void*>(1));testing::close(reinterpret_cast<void*>(1));log=drain();
    require(cleaned==2 && closed==2 && log.find("lua_lifetime_event")==std::string::npos && log.find("\"read_failures\":0")!=std::string::npos,"stopped hooks forward without reads");

    reset();auto* freed=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    require(freed!=nullptr,"allocate VM");put(freed,0x18,global);observe(freed,101,9);
    release_vm=true;testing::close(freed);log=drain();
    require(log.find("vm_close_end")!=std::string::npos && log.find("\"read_failures\":0")!=std::string::npos,"close never reads freed VM");
    reset();freed=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    require(freed!=nullptr,"allocate script state");put(freed,8,vm.data());observe(vm.data(),101,9);
    release_script=true;cleanup(freed);log=drain();
    require(log.find("owner_cleanup_end")!=std::string::npos && log.find("\"read_failures\":0")!=std::string::npos,"cleanup never reads freed state");

    reset();observe(vm.data(),101,10);cleanup(script.data());log=drain();
    require(cleaned==1 && log.find("owner_cleanup_begin")==std::string::npos,"unselected owner cleanup only counted");
    reset();testing::close(vm.data());log=drain();
    require(closed==1 && log.find("vm_close_begin")==std::string::npos,"unobserved VM close only counted");
    reset();observe(reinterpret_cast<void*>(1),101,9);observe(vm.data(),101,9);log=drain();
    require(log.find("\"incomplete\":true")!=std::string::npos && log.find("\"read_failures\":1")!=std::string::npos && log.find("vm_observed")==std::string::npos,"unreadable identity stops epoch assignment");
    reset();for(uintptr_t i=1;i<=18;++i) {put(vm.data(),0x18,i);observe(vm.data(),101,9);}
    log=drain();require(log.find("\"incomplete\":true")!=std::string::npos && occurrences(log,"vm_observed")==16,"bounded VM ledger fails closed");
    reset();for(uintptr_t i=1;i<=150;++i) observe(vm.data(),i,9);
    log=drain();require(occurrences(log,"lua_lifetime_event")==256 && log.find("\"incomplete\":true")!=std::string::npos,"bounded event buffer exposes loss");
    std::cout<<"Lua lifetime checks passed\n";
}
