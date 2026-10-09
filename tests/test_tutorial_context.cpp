#include "tutorial_context.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace crml::tutorial;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class T> void put(std::vector<unsigned char>& b,size_t offset,T value) {std::memcpy(b.data()+offset,&value,sizeof(value));}
uintptr_t address(const std::vector<unsigned char>& b) {return reinterpret_cast<uintptr_t>(b.data());}
struct Fixture {
    static constexpr uintptr_t image=0x140000000;
    static constexpr uint64_t entity=(uint64_t{7}<<32)|2;
    static constexpr uint16_t system_id=23,archetype=1;
    static constexpr uint32_t row=2;
    std::vector<unsigned char> world=std::vector<unsigned char>(0x58600),system=std::vector<unsigned char>(0x150);
    std::vector<unsigned char> meta=std::vector<unsigned char>(16),generations=std::vector<unsigned char>(32),locations=std::vector<unsigned char>(32);
    std::vector<unsigned char> hashes=std::vector<unsigned char>(28),offsets=std::vector<unsigned char>(28),chunk=std::vector<unsigned char>(4096);
    uintptr_t world_view{};
    std::array<uintptr_t,9> query{};
    Fixture() {
        world_view=address(world);
        put(system,0,system_id);put(system,0x140,image+request_dispatch_rva);put(system,0x148,image+request_job_rva);
        put(world,0x58478,address(meta));put(meta,8,uint64_t{2});
        put(world,0x58510,uint64_t{4});put(world,0x584e8,address(generations));put(generations,16,uint32_t{7});
        put(world,0x58530,address(locations));put(locations,16,(uint64_t{row}<<32)|archetype);
        put(world,0x58,address(chunk));put(world,0x1044c,row+1);put(chunk,0x10+row*8,entity);
        constexpr uint32_t h[]{0xed2c12b8,0x48941389,0x12707f01,0x9fc946e4,0xae510289,0x4a56ea3c,0x423c371a};
        put(world,(archetype+0xc22)*32+0x10,address(hashes));put(world,(archetype+0xc22)*32+0x24,uint32_t{7});
        put(world,0x18478,address(offsets));
        for(size_t i=0;i<7;++i) {
            const auto offset=static_cast<uint32_t>(0x100+i*0x200);
            put(hashes,i*4,h[i]);put(offsets,i*4,offset);query[i]=address(chunk)+offset;
        }
        query[7]=address(chunk);query[8]=row;
    }
    IdentityStatus identify(Identity& identity) {return RequestScope::identify(query.data(),identity);}
};
void rejected(Fixture& f,const char* message) {
    Identity id{123,456};require(f.identify(id)!=IdentityStatus::ok && !id.world && !id.entity,message);
}
struct NativeVector {uintptr_t data;uint32_t size,capacity;};
struct NativeRequest {uint32_t kind_and_padding,id;};
struct NativeSelection {uint32_t id;int32_t priority;float time;};
uintptr_t component(Fixture& f,size_t slot) {return f.query[slot]+f.row*(slot==4?0x70:16);}
void set_vector(Fixture& f,size_t slot,void* data,uint32_t size,uint32_t capacity) {
    const NativeVector v{reinterpret_cast<uintptr_t>(data),size,capacity};
    std::memcpy(reinterpret_cast<void*>(component(f,slot)),&v,sizeof(v));
}
NativeVector get_vector(Fixture& f,size_t slot) {
    NativeVector v{};std::memcpy(&v,reinterpret_cast<void*>(component(f,slot)),sizeof(v));return v;
}
void withdrawal_cases() {
    Fixture f;constexpr uint32_t key=42;
    NativeRequest requests[]{{0x12340000,11},{3,key},{0x87654304,22},{0,key},{4,key}};
    uint32_t completions[]{key,22,key,11};
    NativeSelection selection[]{{key,30,1.f},{22,20,2.f},{key,10,3.f},{11,-1,4.f}};
    uint32_t active[]{11,key,22},completed[]{7,8,9},dismissed[]{9,8,7};
    const Identity expected{f.world_view,f.entity};
    set_vector(f,0,completed,3,3);set_vector(f,1,requests,5,5);set_vector(f,2,active,3,3);
    set_vector(f,3,dismissed,3,3);set_vector(f,5,completions,4,4);set_vector(f,6,selection,4,4);
    const auto data=component(f,4);std::memcpy(reinterpret_cast<void*>(data),&key,4);
    *reinterpret_cast<unsigned char*>(data+4)=1;
    *reinterpret_cast<unsigned char*>(data+0x69)=0x5a;
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::scope,"withdrawal requires a live dispatcher scope");
    RequestScope scope(&f.world_view,f.system_id,f.system.data(),Dispatch::direct,f.image);
    auto foreign=expected;++foreign.entity;
    require(RequestScope::withdraw(f.query.data(),foreign,key).status==WithdrawalStatus::identity && get_vector(f,1).size==5,"foreign/stale entity cannot withdraw requests");
    auto result=RequestScope::withdraw(f.query.data(),expected,99);
    require(result.status==WithdrawalStatus::ok && !result.requests && !result.completions && !result.selections && !result.active && !result.selected,"absent owned key is a no-op");
    require(RequestScope::withdraw(f.query.data(),expected,0).status==WithdrawalStatus::invalid,"zero owner key rejected");
    // A malformed later queue must reject before any earlier queue changes.
    set_vector(f,6,selection,4,3);
    result=RequestScope::withdraw(f.query.data(),expected,key);
    require(result.status==WithdrawalStatus::invalid && get_vector(f,1).size==5 && requests[1].id==key,"validate every queue before mutation");
    set_vector(f,6,selection,4,4);
    set_vector(f,0,requests,3,3);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::invalid && requests[1].id==key,"reject buffer alias with native completion progress");
    set_vector(f,0,completed,3,3);
    set_vector(f,5,reinterpret_cast<void*>(data),4,4);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::invalid,"reject payload overlapping component header/data");
    set_vector(f,5,completions,4,4);
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    require(memory!=nullptr,"withdrawal guard allocation");
    std::memcpy(memory,selection,sizeof(selection));DWORD old{};
    const bool protected_page=VirtualProtect(memory,4096,PAGE_READONLY,&old)!=0;
    if(!protected_page) {VirtualFree(memory,0,MEM_RELEASE);throw std::runtime_error("withdrawal guard protection");}
    set_vector(f,6,memory,4,4);
    result=RequestScope::withdraw(f.query.data(),expected,key);
    const bool unchanged=result.status==WithdrawalStatus::unwritable && get_vector(f,1).size==5 && requests[1].id==key && get_vector(f,5).size==4;
    DWORD previous{};
    const bool guard_set=VirtualProtect(memory,4096,PAGE_READWRITE|PAGE_GUARD,&previous)!=0;
    const auto guarded=RequestScope::withdraw(f.query.data(),expected,key);
    MEMORY_BASIC_INFORMATION info{};const auto queried=VirtualQuery(memory,&info,sizeof(info));
    const bool guard_preserved=guard_set && guarded.status==WithdrawalStatus::invalid && queried && (info.Protect&PAGE_GUARD);
    const bool noaccess_set=VirtualProtect(memory,4096,PAGE_NOACCESS,&previous)!=0;
    const auto inaccessible=RequestScope::withdraw(f.query.data(),expected,key);
    VirtualFree(memory,0,MEM_RELEASE);set_vector(f,6,selection,4,4);
    require(unchanged,"unwritable late queue cannot partially compact earlier queues");
    require(guard_preserved && noaccess_set && inaccessible.status==WithdrawalStatus::invalid && requests[1].id==key,"guard and no-access payloads rejected without touching or changing their protection");
    set_vector(f,5,nullptr,4,4);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::invalid && requests[1].id==key,"nonempty null payload rejected before mutation");
    set_vector(f,5,completions,4,4);
    auto* crossing=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    require(crossing!=nullptr,"cross-region allocation");
    std::memcpy(crossing+4096-12,selection,12);
    const bool crossing_protected=VirtualProtect(crossing+4096,4096,PAGE_NOACCESS,&previous)!=0;
    set_vector(f,6,crossing+4096-12,2,2);
    const auto crossing_result=RequestScope::withdraw(f.query.data(),expected,key);
    VirtualFree(crossing,0,MEM_RELEASE);set_vector(f,6,selection,4,4);
    require(crossing_protected && crossing_result.status==WithdrawalStatus::invalid && requests[1].id==key,"payload spanning readable and unreadable regions rejected before mutation");
    result=RequestScope::withdraw(f.query.data(),expected,key);
    require(result.status==WithdrawalStatus::ok && result.requests==3 && result.completions==2 && result.selections==2,"all matching queued references removed");
    require(result.active && result.selected,"withdrawal explicitly reports remaining active/selected references");
    require(get_vector(f,1).size==2 && get_vector(f,1).capacity==5 && get_vector(f,1).data==reinterpret_cast<uintptr_t>(requests),"native allocation/capacity preserved");
    require(requests[0].id==11 && requests[1].id==22 && requests[1].kind_and_padding==0x87654304,"unrelated request order and opaque padding preserved");
    require(get_vector(f,5).size==2 && completions[0]==22 && completions[1]==11,"completion queue order preserved");
    require(get_vector(f,6).size==2 && selection[0].id==22 && selection[0].priority==20 && selection[0].time==2.f && selection[1].id==11 && selection[1].priority==-1 && selection[1].time==4.f,"selection priority/time/order preserved");
    require(active[1]==key && get_vector(f,2).size==3 && completed[0]==7 && dismissed[0]==9 &&
        *reinterpret_cast<unsigned char*>(data+4)==1 && *reinterpret_cast<unsigned char*>(data+0x69)==0x5a,"no activation, selection, progression or change-flag writes");
    result=RequestScope::withdraw(f.query.data(),expected,key);
    require(result.status==WithdrawalStatus::ok && !result.requests && !result.completions && !result.selections && result.active && result.selected,"repeated withdrawal is no-op and never claims presentation retirement");
    set_vector(f,1,requests,0,5);set_vector(f,5,nullptr,0,0);set_vector(f,6,selection,0,4);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::ok,"empty allocated and unallocated vectors supported");
    std::vector<uint32_t> bounded(4097,99);
    set_vector(f,5,bounded.data(),4096,4097);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::ok,"queue bound permits4096 entries");
    set_vector(f,5,bounded.data(),4097,4097);
    require(RequestScope::withdraw(f.query.data(),expected,key).status==WithdrawalStatus::invalid,"queue bound rejects4097 entries");
    set_vector(f,5,nullptr,0,0);
    auto* readonly=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    require(readonly!=nullptr,"readonly no-op allocation");
    const uint32_t foreign_key=99;std::memcpy(readonly,&foreign_key,4);
    const bool made_readonly=VirtualProtect(readonly,4096,PAGE_READONLY,&previous)!=0;
    set_vector(f,5,readonly,1,1);
    const auto readonly_result=RequestScope::withdraw(f.query.data(),expected,key);
    VirtualFree(readonly,0,MEM_RELEASE);
    require(made_readonly && readonly_result.status==WithdrawalStatus::ok && !readonly_result.completions,"readonly queue without matches is a successful no-op");
}
}
int main() {
    try {
        withdrawal_cases();
        Fixture f;Identity id{123,456};
        require(f.identify(id)==IdentityStatus::unavailable && !id.entity && !id.world,"no cached world outside dispatcher");
        const auto world_before=f.world,chunk_before=f.chunk;
        {
            RequestScope direct(&f.world_view,f.system_id,f.system.data(),Dispatch::direct,f.image);
            require(f.identify(id)==IdentityStatus::ok && id.entity==f.entity && id.world==f.world_view,"full entity generation resolves from direct query");
            {
                RequestScope job(&f.world_view,f.archetype,f.system.data(),Dispatch::archetype,f.image);
                require(f.identify(id)==IdentityStatus::ok,"archetype dispatcher uses archetype instead of system ID");
            }
            for(size_t i=0;i<7;++i) {
                const auto base=f.query[i];f.query[i]+=16;rejected(f,"every component base must match current entity");f.query[i]=base;
                f.query[i]=UINTPTR_MAX-4;rejected(f,"wrapping component extent rejected");f.query[i]=base;
                const auto hash=*reinterpret_cast<const uint32_t*>(f.hashes.data()+i*4);
                put(f.hashes,i*4,uint32_t{0});rejected(f,"every declared component required");put(f.hashes,i*4,hash);
            }
            put(f.generations,16,uint32_t{8});rejected(f,"stale generation rejected");put(f.generations,16,uint32_t{7});
            put(f.locations,16,(uint64_t{f.row+1}<<32)|1);rejected(f,"relocated entity rejected");put(f.locations,16,(uint64_t{f.row}<<32)|1);
            put(f.world,0x58,address(f.chunk)+8);rejected(f,"replaced chunk rejected");put(f.world,0x58,address(f.chunk));
            put(f.world,0x1044c,f.row);rejected(f,"row outside current chunk count rejected");put(f.world,0x1044c,f.row+1);
            put(f.world,0x58510,uint64_t{2});rejected(f,"entity index outside capacity rejected");put(f.world,0x58510,uint64_t{4});
            f.query[8]=16384;rejected(f,"row cap enforced before header read");f.query[8]=f.row;
            for(const auto handle:{uint64_t{0},UINT64_MAX}) {put(f.chunk,0x10+f.row*8,handle);rejected(f,"invalid full handles rejected");}
            put(f.chunk,0x10+f.row*8,f.entity);
            {
                RequestScope foreign(&f.world_view,f.system_id+1,f.system.data(),Dispatch::direct,f.image);
                require(f.identify(id)==IdentityStatus::scope,"foreign direct system ID shadows outer scope");
            }
            {
                RequestScope foreign(&f.world_view,0,f.system.data(),Dispatch::archetype,f.image);
                require(f.identify(id)==IdentityStatus::scope,"job cannot identify another archetype");
            }
            const auto old_world=f.world_view;f.world_view=0;rejected(f,"world replacement invalidates active scope");f.world_view=old_world;
            put(f.system,0x148,uintptr_t{1});rejected(f,"system callback mutation invalidates active scope");put(f.system,0x148,f.image+request_job_rva);
            bool shadowed=false;
            try {
                RequestScope invalid(nullptr,0,nullptr,Dispatch::direct,f.image);
                shadowed=f.identify(id)==IdentityStatus::scope;
                throw std::runtime_error("unwind nested dispatcher");
            } catch(const std::runtime_error&) {}
            require(shadowed && f.identify(id)==IdentityStatus::ok,"invalid nested scope shadows outer world, restored after unwind");
            Fixture other;
            {
                RequestScope nested(&other.world_view,other.system_id,other.system.data(),Dispatch::direct,other.image);
                require(other.identify(id)==IdentityStatus::ok && id.world==other.world_view,"nested valid world replaces outer world");
                rejected(f,"outer query cannot resolve in nested world");
            }
            require(f.identify(id)==IdentityStatus::ok && id.world==f.world_view,"outer world restored after nested return");
            std::atomic<bool> thread_ok{};
            std::thread thread([&] {
                Identity result{};
                bool ok=f.identify(result)==IdentityStatus::unavailable;
                RequestScope own(&other.world_view,other.system_id,other.system.data(),Dispatch::direct,other.image);
                ok=ok && other.identify(result)==IdentityStatus::ok && result.world==other.world_view;
                thread_ok.store(ok);
            });thread.join();
            require(thread_ok.load() && f.identify(id)==IdentityStatus::ok,"scopes are isolated across threads");
            auto* guard=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            require(guard!=nullptr,"guard allocation");DWORD previous{};
            const bool protected_page=VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&previous)!=0;
            if(!protected_page) {VirtualFree(guard,0,MEM_RELEASE);throw std::runtime_error("guard protection");}
            const auto result=RequestScope::identify(guard+4096-64,id);
            const auto old=f.query[7];f.query[7]=reinterpret_cast<uintptr_t>(guard+4096);
            const auto header_result=f.identify(id);f.query[7]=old;
            VirtualFree(guard,0,MEM_RELEASE);
            require(result==IdentityStatus::memory && header_result==IdentityStatus::memory && !id.world && !id.entity,"unreadable query/header return no partial identity");
            require(f.world==world_before && f.chunk==chunk_before,"identity lookup performs no engine writes");
        }
        require(f.identify(id)==IdentityStatus::unavailable,"return removes borrowed scope");
        std::cout<<"Tutorial context tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
