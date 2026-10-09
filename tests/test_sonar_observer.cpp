#include "diagnostics/sonar_observer.cpp"
#include <iostream>
#include <stdexcept>
using namespace crml::sonar_observer;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
unsigned geometry_calls{},worker_calls{},facts_calls{},snapshots{};
bool throw_geometry{},throw_worker{},corrupt_geometry{},mutate_worker{};
crml::sonar_projection::Snapshot received{};
void receive(const crml::sonar_projection::Snapshot& snapshot,uint64_t) noexcept {++snapshots;received=snapshot;}
void* fake_geometry(void* out,const void* reference,const void* plane,const void* origin,
                    const void* target,float near_radius,float far_radius,uint8_t flag) {
    ++geometry_calls;
    if(throw_geometry)RaiseException(0xe0421234,0,0,nullptr);
    crml::sonar_projection::Frame f{};
    read_frame(f,reference,plane,origin,target,near_radius,far_radius,flag);
    crml::sonar_projection::Model m{};
    crml::sonar_projection::model_from_frame(f,m);
    std::array<float,2> uv{};float height{},radial{},scale{};
    crml::sonar_projection::project_model(m,{f.target[0],f.target[1],f.target[2]},uv,height,&radial,&scale);
    auto local=crml::sonar_projection::local_displacement(m.plane,m.origin,{f.target[0],f.target[1],f.target[2]});
    local[1]=0;const auto planar=crml::sonar_projection::rotate(m.plane,local);
    std::array<unsigned char,0x30> bytes{};
    const auto radial_class=crml::sonar_projection::distance_class(radial,near_radius,far_radius);
    const auto vertical_class=crml::sonar_projection::distance_class(std::abs(height),near_radius,far_radius);
    const std::array<float,4> flattened{m.origin[0]+planar[0],m.origin[1]+planar[1],m.origin[2]+planar[2],0};
    std::memcpy(bytes.data(),&radial_class,4);std::memcpy(bytes.data()+4,&vertical_class,4);
    std::memcpy(bytes.data()+0x10,flattened.data(),16);std::memcpy(bytes.data()+0x20,&radial,4);
    std::memcpy(bytes.data()+0x24,&scale,4);std::memcpy(bytes.data()+0x28,uv.data(),8);
    if(corrupt_geometry){const float bad=uv[0]+.1f;std::memcpy(bytes.data()+0x28,&bad,4);}
    std::memcpy(out,bytes.data(),bytes.size());return out;
}
void fake_worker(const void* row,const void*,const void*,const void*) {
    ++worker_calls;if(throw_worker)RaiseException(0xe0421234,0,0,nullptr);
    if(mutate_worker)reinterpret_cast<float*>(static_cast<const uint64_t*>(row)[0])[4]+=1;
}
void fake_facts(const void*,const void*,const void*,const void*,const void*,const void*,const float*) {++facts_calls;}
bool geometry_unwind(void* out,const float* q,const float* p,const float* o,const float* t,const void* slot) {
    __try {geometry_call(out,q,p,o,t,20,80,0,image+0x1e77ddd,slot);return false;}
    __except(GetExceptionCode()==0xe0421234?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
}
bool worker_unwind(const void* row,const void* sonar) {
    __try {worker_call(row,sonar,nullptr,nullptr,image+0x1e71fde);return false;}
    __except(GetExceptionCode()==0xe0421234?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
}
int main(){try {
    require(MH_Initialize()==MH_OK,"initialize MinHook");
    for(const auto prefix:{std::pair{geometry_bytes,sizeof(geometry_bytes)},
                           std::pair{worker_bytes,sizeof(worker_bytes)},
                           std::pair{facts_bytes,sizeof(facts_bytes)}}) {
        auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        require(memory!=nullptr,"allocate prologue");
        std::memset(memory,0x90,4096);std::memcpy(memory,prefix.first,prefix.second);
        void* original{};const auto status=MH_CreateHook(memory,reinterpret_cast<void*>(&fake_geometry),&original);
        if(status==MH_OK)MH_RemoveHook(memory);VirtualFree(memory,0,MEM_RELEASE);
        require(status==MH_OK&&original,"native sonar prologue accepted");
    }
    MH_Uninitialize();
    original_geometry=fake_geometry;original_worker=fake_worker;original_facts=fake_facts;
    auto* base=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x5c75000,MEM_RESERVE,PAGE_NOACCESS));
    require(base!=nullptr,"reserve image fixture");
    require(VirtualAlloc(base+0x5c74000,4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit radius globals");
    image=reinterpret_cast<uintptr_t>(base);admission=0;snapshot_callback=receive;
    const float near_radius=20,far_radius=80;
    std::memcpy(base+0x5c74578,&near_radius,4);std::memcpy(base+0x5c745c8,&far_radius,4);
    float current[8]{},previous[8]{},camera[0xb0/4]{},plane[4]{0,0,0,1};
    current[4]=100;current[5]=20;current[6]=200;
    previous[4]=99;previous[5]=20;previous[6]=200;
    camera[0x60/4+3]=1;camera[0x80/4+1]=camera[0x80/4+3]=0.70710678f;
    const std::array<uint64_t,6> row{reinterpret_cast<uint64_t>(current),reinterpret_cast<uint64_t>(previous),
        reinterpret_cast<uint64_t>(camera),reinterpret_cast<uint64_t>(plane),0,0};
    std::array<uint8_t,0x81> sonar{};sonar[0]=3;
    float blend=.6f;facts_call(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&blend,image+0x1e70fca);
    require(facts_calls==1&&interpolation.load()==blend,"facts hook copies live interpolation");
    next_sample=0;worker_call(row.data(),sonar.data(),nullptr,nullptr,image+0x1e71fde);
    require(worker_calls==1&&snapshots==1&&received.interpolation==blend&&
            received.current.origin[0]==100&&received.previous.origin[0]==99&&
            received.current.reference[3]==1&&received.previous.reference[1]==camera[0x80/4+1],
            "marker-free player worker publishes copied current and previous frames");
    worker_call(row.data(),sonar.data(),nullptr,nullptr,0);
    require(worker_calls==2&&snapshots==1,"other worker caller forwarded without publication");
    float q[4]{0,0,0,1},target[4]{110,23,200,0},out[0x30/4]{};
    std::array<unsigned char,0x1d0> caller_stack{};
    const uintptr_t outer=image+0x1e68ed3;
    std::memcpy(caller_stack.data()+0x1c0,&outer,8);
    next_geometry_sample=0;
    geometry_call(out,q,plane,current+4,target,20,80,0,image+0x1e77ddd,caller_stack.data());
    require(pending.valid&&geometry_calls==1,"native first geometry result compared");
    geometry_call(out,q,plane,previous+4,target,20,80,0,image+0x1e77e6a,caller_stack.data());
    require(matched==1&&!math_mismatch&&geometry_calls==2,"same-wrapper second geometry qualifies math");
    throw_geometry=true;next_geometry_sample=0;
    require(geometry_unwind(out,q,plane,current+4,target,caller_stack.data())&&admission==0&&unwinds==1,
            "geometry native exception propagates and releases admission");
    throw_geometry=false;throw_worker=true;
    require(worker_unwind(row.data(),sonar.data())&&admission==0&&unwinds==2,
            "worker native exception propagates and releases admission");
    facts_call(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&blend,image+0x1e70fca);
    throw_worker=false;mutate_worker=true;next_sample=0;
    worker_call(row.data(),sonar.data(),nullptr,nullptr,image+0x1e71fde);
    require(snapshots==1,"component mutation during callthrough cannot publish mixed frame");
    mutate_worker=false;current[4]=100;corrupt_geometry=true;next_geometry_sample=0;
    geometry_call(out,q,plane,current+4,target,20,80,0,image+0x1e77ddd,caller_stack.data());
    require(math_mismatch.load(),"disagreeing native UV latches math failure");
    facts_call(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&blend,image+0x1e70fca);
    corrupt_geometry=false;next_sample=0;
    worker_call(row.data(),sonar.data(),nullptr,nullptr,image+0x1e71fde);
    require(snapshots==1,"marker-free heartbeat cannot bypass native math failure");
    stop();const auto prior=worker_calls;
    worker_call(row.data(),sonar.data(),nullptr,nullptr,image+0x1e71fde);
    require(worker_calls==prior+1&&snapshots==1,"stopped hook still forwards native worker");
    VirtualFree(base,0,MEM_RELEASE);
    std::cout<<"Sonar observer checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
