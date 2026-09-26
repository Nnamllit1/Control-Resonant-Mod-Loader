#include "engine_observer.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>
using namespace crml::observer;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        require(test_hook_prologues(),"all seven observed prologues must be relocatable by MinHook");
        auto buffer=std::make_unique<Buffer>(); buffer->open();
        for(size_t i=0;i<Buffer::capacity;++i) { Event e{}; e.value=i; require(buffer->push(e),"initial buffer fill"); }
        require(!buffer->push({}) && buffer->dropped()==1,"full-buffer loss must be explicit");
        std::array<Event,128> out{}; size_t read=0;
        while(const auto size=buffer->drain(out.data(),out.size())) {
            for(size_t i=0;i<size;++i) { require(out[i].sequence==read+1 && out[i].value==read,"FIFO/order across drains"); ++read; }
        }
        require(read==Buffer::capacity,"complete drain"); buffer->close();
        require(!buffer->push({}),"closed buffer accepts no records");
        Event invalid{}; invalid.kind=Kind::count; require(!buffer->push(invalid),"invalid kind rejected");

        buffer->open(); std::atomic<unsigned> finished{}; std::vector<std::thread> writers;
        for(unsigned thread=0;thread<4;++thread) writers.emplace_back([&,thread] {
            for(unsigned i=0;i<10000;++i) { Event e{}; e.kind=Kind::resource; e.value=(uint64_t(thread)<<32)|i; buffer->push(e); }
            ++finished;
        });
        std::unordered_set<uint64_t> values; uint64_t sequence=0;
        auto drain=[&] {
            const auto size=buffer->drain(out.data(),out.size());
            for(size_t i=0;i<size;++i) {
                require(out[i].sequence==++sequence,"concurrent publication order");
                require(values.insert(out[i].value).second,"concurrent duplicate record");
            }
            return size;
        };
        while(finished.load()!=4) { drain(); std::this_thread::yield(); }
        for(auto& thread:writers) thread.join(); while(drain()) {}
        require(sequence+buffer->dropped()==40000,"all producer records accounted for");
        require(buffer->count(Kind::resource)==40000,"attempt count"); buffer->close();

        std::array<unsigned char,128> resource{};
        uint64_t id=0x123456789abcdef0ull; uint32_t refs=17,state=4;
        std::memcpy(resource.data()+8,&id,8); std::memcpy(resource.data()+0x64,&refs,4); std::memcpy(resource.data()+0x68,&state,4);
        const auto copy=resource; const auto sample=read_resource(reinterpret_cast<uintptr_t>(resource.data()));
        require(sample.readable && sample.id==id && sample.refs==17 && sample.state==4,"copied resource fields");
        require(resource==copy,"observation must not write resource memory");
        require(!read_resource(0).readable && !read_resource(1).readable,"invalid resource pointers");
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        auto pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,system.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        require(pages!=nullptr,"guarded allocation"); DWORD old{};
        require(VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old)!=0,"guard page");
        const auto partial=read_resource(reinterpret_cast<uintptr_t>(pages+system.dwPageSize-0x60));
        VirtualFree(pages,0,MEM_RELEASE);
        require(!partial.readable && !partial.pointer && !partial.id,"partial read must fail as a whole");
        require(identity(0,55)==0 && identity(1234,55)==identity(1234,55) && identity(1234,55)!=identity(1234,56),"session identities");
        Event e{}; e.kind=Kind::resource; e.object=UINT64_MAX; e.entity=UINT64_MAX;
        std::ostringstream text; write_event(text,e);
        require(text.str().find("\"18446744073709551615\"")!=std::string::npos,"64-bit identities must be JSON strings");
        std::cout<<"Observer buffer concurrency, loss accounting, guarded snapshots and serialization passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
