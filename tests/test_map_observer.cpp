#include "diagnostics/map_observer.cpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace crml::map_observer;
void require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
unsigned invoked{};
bool throwing{},mutating{};
bool bad_rectangle{},throwing_rectangle{};
unsigned snapshots{};
crml::map_projection::District last_projection{};
std::array<float,4> last_rectangle{};
uint64_t last_time{};
void receive_snapshot(const crml::map_projection::District& projection,const std::array<float,4>& rectangle,uint64_t now) noexcept {
    ++snapshots;last_projection=projection;last_rectangle=rectangle;last_time=now;
}
void* fake_project(void* out,const void* point,const void* district) {
    ++invoked;
    if(throwing)RaiseException(0xe0421234,0,0,nullptr);
    const float transform[8]{0,0,0,1,0,0,0,0};
    capture_transform(const_cast<float*>(transform),district,image+0x1f52e32);
    const auto* p=static_cast<const float*>(point);
    auto* result=static_cast<float*>(out);result[0]=p[0]/100;result[1]=1-p[1]/400;
    if(mutating)reinterpret_cast<float*>(const_cast<void*>(district))[0x74/4]+=1;
    return out;
}
void* fake_rectangle(void* out,const void*) {
    if(throwing_rectangle)RaiseException(0xe0421234,0,0,nullptr);
    auto* result=static_cast<float*>(out);
    result[0]=10;result[1]=20;result[2]=bad_rectangle?-1.f:300.f;result[3]=400;
    return out;
}
bool unwind(void* out,const void* point,const void* district) {
    __try {project_call(out,point,district,image+0x1f53945,out);return false;}
    __except(GetExceptionCode()==0xe0421234?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
}
bool unwind_rectangle(void* out,const void* district,const void* caller_slot) {
    __try {rectangle_call(out,district,image+0x1f53985,caller_slot);return false;}
    __except(GetExceptionCode()==0xe0421234?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
}
int main(){try {
    require(MH_Initialize()==MH_OK,"initialize hook library");
    for(const auto prefix:{std::pair{project_bytes,sizeof(project_bytes)},std::pair{transform_bytes,sizeof(transform_bytes)},std::pair{rectangle_bytes,sizeof(rectangle_bytes)}}) {
        auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        require(memory!=nullptr,"allocate prologue fixture");
        std::memset(memory,0x90,4096);std::memcpy(memory,prefix.first,prefix.second);
        void* original{};
        const auto status=MH_CreateHook(memory,reinterpret_cast<void*>(&fake_project),&original);
        if(status==MH_OK)MH_RemoveHook(memory);
        VirtualFree(memory,0,MEM_RELEASE);
        require(status==MH_OK && original,"actual map prologue must be accepted by hook decoder");
    }
    MH_Uninitialize();
    original_project=fake_project;original_rectangle=fake_rectangle;image=0x10000000;admission=0;deadline=GetTickCount64()+60000;
    std::array<float,40> district{};district[0x80/4]=100;district[0x84/4]=200;district[0x88/4]=50;
    district[0x94/4]=district[0x98/4]=1;
    float point[3]{50,200,0},out[2]{};
    require(project_call(out,point,district.data(),image+0x1f53945,out)==out && invoked==1,"transparent callthrough and result");
    require(size==1 && queue[0].sequence==1 && queue[0].compared && queue[0].matched,"native output comparison");
    require(!frame && admission==0,"TLS and admission released");
    next_sample=0;mutating=true;project_call(out,point,district.data(),image+0x1f53945,out);mutating=false;
    require(size==2 && !queue[1].stable && !queue[1].compared,"changing district rejected");
    district[0x74/4]-=1;
    next_sample=0;throwing=true;Frame prior{};frame=&prior;
    require(unwind(out,point,district.data()) && frame==&prior && admission==0 && unwinds==1,"native exception propagated with TLS cleanup");
    frame=nullptr;throwing=false;next_sample=0;
    auto before=used.load();project_call(out,point,district.data(),0,out);
    require(used==before && unrecognized==1,"other callers forwarded without capture");
    used=511;next_sample=0;require(sample(100)==512,"last sample has unique reservation");next_sample=0;
    require(!sample(200) && used==512,"bounded sample budget");
    head=size=0;for(unsigned i=0;i<70;++i)enqueue(Event{});
    require(size==64 && dropped==6,"bounded nonblocking queue");
    Frame invalid{};invalid.event.readable=true;invalid.event.sequence=999;invalid.district=district.data();
    begin(invalid,point,district.data(),100);invalid.event.transform=true;invalid.projection.rotation={0,0,0,1};
    head=size=0;finish(invalid,reinterpret_cast<void*>(1),point);
    require(size==1 && queue[0].predicted && !queue[0].compared,"unreadable output never counts as comparison");
    head=size=0;next_sample=0;deadline=0;diagnostic_enabled=true;
    snapshot_callback=receive_snapshot;next_snapshot=0;
    std::array<unsigned char,0xc0> caller_stack{};
    auto* native_slot=caller_stack.data();
    uintptr_t outer_return=image+0x1f541e6;
    std::memcpy(caller_stack.data()+0xb0,&outer_return,sizeof(outer_return));
    require(project_call(out,point,district.data(),image+0x1f53945,native_slot)==out && pending.valid && size==0,
            "production capture survives diagnostic deadline without queueing");
    float rectangle[4]{};
    require(rectangle_call(rectangle,district.data(),image+0x1f53985,native_slot)==rectangle && snapshots==1 &&
            last_rectangle==std::array<float,4>{10,20,300,400} && last_projection.maximum[0]==100 && last_time,
            "copied district and native rectangle delivered once");
    rectangle_call(rectangle,district.data(),image+0x1f53985,native_slot);
    require(snapshots==1,"native rectangle cannot consume pending snapshot twice");
    outer_return=0;std::memcpy(caller_stack.data()+0xb0,&outer_return,sizeof(outer_return));
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    require(!pending.valid && snapshots==1,"other marker append callers never publish player context");
    outer_return=image+0x1f541e6;std::memcpy(caller_stack.data()+0xb0,&outer_return,sizeof(outer_return));
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,reinterpret_cast<const void*>(1));
    require(!pending.valid,"unreadable outer return address rejected");
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    rectangle_call(rectangle,district.data(),image+0x1f53985,point);
    require(snapshots==1,"different caller stack slot rejected");
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    rectangle_call(rectangle,point,image+0x1f53985,native_slot);
    require(snapshots==1,"different district identity rejected");
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    rectangle_call(rectangle,district.data(),0,native_slot);
    require(snapshots==1,"other rectangle callers rejected");
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    bad_rectangle=true;rectangle_call(rectangle,district.data(),image+0x1f53985,native_slot);bad_rectangle=false;
    require(snapshots==1,"invalid rectangle rejected");
    next_snapshot=0;project_call(out,point,district.data(),image+0x1f53945,native_slot);
    throwing_rectangle=true;require(unwind_rectangle(rectangle,district.data(),native_slot) && admission==0 && !pending.valid,
                                    "rectangle exception propagates and releases admission");
    throwing_rectangle=false;
    next_snapshot=0;mutating=true;project_call(out,point,district.data(),image+0x1f53945,native_slot);mutating=false;
    require(!pending.valid,"unstable district cannot publish copied snapshot");
    installed=true;stop();before=used;project_call(out,point,district.data(),image+0x1f53945,out);
    require(used==before && !frame,"stopped observer still forwards calls");
    enqueue(invalid.event);
    std::ostringstream log;log<<std::hex;poll(log);
    require(log.str().find("\"compared\":false")!=std::string::npos && log.str().find("\"status\":\"stopped\"")!=std::string::npos,"explicit comparison and completion reporting");
    require((log.flags()&std::ios::basefield)==std::ios::hex,"stream flags restored");
    std::cout<<"Map observer checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
