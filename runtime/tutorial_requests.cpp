#include "tutorial_requests.h"
#include <Windows.h>
#include <cstring>
#include <limits>

namespace crml::tutorial {
namespace {
struct Vector {uintptr_t data;uint32_t size,capacity;};
static_assert(sizeof(Vector)==16);
struct Queue {uintptr_t header;Vector vector;uint32_t stride,key_offset;};
bool extent(uintptr_t start,size_t bytes) noexcept {
    return start && bytes && start<=std::numeric_limits<uintptr_t>::max()-bytes;
}
bool overlap(uintptr_t a,size_t an,uintptr_t b,size_t bn) noexcept {
    return an && bn && a<b+bn && b<a+an;
}
bool accessible(uintptr_t start,size_t bytes,bool write) noexcept {
    if(!extent(start,bytes)) return false;
    const auto end=start+bytes;
    while(start<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(start),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto protection=info.Protect&0xff;
        const bool can_write=protection==PAGE_READWRITE || protection==PAGE_WRITECOPY ||
                             protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
        if(!can_write && (write || (protection!=PAGE_READONLY && protection!=PAGE_EXECUTE_READ))) return false;
        const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(!extent(base,info.RegionSize) || base+info.RegionSize<=start) return false;
        start=base+info.RegionSize;
    }
    return true;
}
uint32_t id_at(const Queue& q,uint32_t i) noexcept {
    uint32_t id{};std::memcpy(&id,reinterpret_cast<void*>(q.vector.data+i*static_cast<uintptr_t>(q.stride)+q.key_offset),4);return id;
}
uint32_t compact(const Queue& q,uint32_t key) noexcept {
    uint32_t kept=0;
    for(uint32_t i=0;i<q.vector.size;++i) {
        if(id_at(q,i)==key) continue;
        if(kept!=i) std::memmove(reinterpret_cast<void*>(q.vector.data+kept*static_cast<uintptr_t>(q.stride)),
                               reinterpret_cast<void*>(q.vector.data+i*static_cast<uintptr_t>(q.stride)),q.stride);
        ++kept;
    }
    if(kept!=q.vector.size) std::memcpy(reinterpret_cast<void*>(q.header+8),&kept,4);
    return q.vector.size-kept;
}
}
Withdrawal withdraw_queued(const std::array<uintptr_t,7>& components,uint32_t key) noexcept {
    Withdrawal result{};bool committing=false;
    __try {
        if(!key) return result;
        for(size_t i=0;i<components.size();++i) {
            if(!accessible(components[i],i==4?0x70:16,false)) return result;
            for(size_t j=0;j<i;++j)
                if(overlap(components[i],i==4?0x70:16,components[j],j==4?0x70:16)) return result;
        }
        Queue queues[]{{components[1],{},8,4},{components[5],{},4,0},{components[6],{},12,0},
                       {components[2],{},4,0},{components[0],{},4,0},{components[3],{},4,0}};
        // Read and validate every queue before writing any. Bounds limit work;
        // buffers/capacities stay engine-owned and are never reallocated here.
        uint32_t matches[3]{};
        for(size_t i=0;i<std::size(queues);++i) {
            auto& q=queues[i];std::memcpy(&q.vector,reinterpret_cast<void*>(q.header),sizeof(Vector));
            if(q.vector.size>q.vector.capacity || q.vector.size>4096) return result;
            const auto bytes=static_cast<size_t>(q.vector.size)*q.stride;
            if(bytes && !accessible(q.vector.data,bytes,false)) return result;
            for(size_t c=0;c<components.size();++c)
                if(overlap(q.vector.data,bytes,components[c],c==4?0x70:16)) return result;
            for(size_t j=0;j<i;++j)
                if(overlap(q.vector.data,bytes,queues[j].vector.data,static_cast<size_t>(queues[j].vector.size)*queues[j].stride)) return result;
            for(uint32_t row=0;row<q.vector.size;++row) {
                const auto candidate=id_at(q,row);
                if(candidate==key) {if(i<3) ++matches[i];else if(i==3) result.active=true;}
            }
        }
        uint32_t selected{};unsigned char valid{};
        std::memcpy(&selected,reinterpret_cast<void*>(components[4]),4);
        std::memcpy(&valid,reinterpret_cast<void*>(components[4]+4),1);
        result.selected=valid && selected==key;
        for(size_t i=0;i<3;++i) {
            const auto& q=queues[i];
            if(matches[i] && (!accessible(q.header+8,4,true) || !accessible(q.vector.data,static_cast<size_t>(q.vector.size)*q.stride,true))) {
                result.status=WithdrawalStatus::unwritable;return result;
            }
        }
        // Native dispatch scope is the synchronization precondition. Page
        // checks are not locks, so an unexpected write fault remains explicit.
        committing=true;
        result.requests=matches[0]?compact(queues[0],key):0;
        result.completions=matches[1]?compact(queues[1],key):0;
        result.selections=matches[2]?compact(queues[2],key):0;
        result.status=WithdrawalStatus::ok;return result;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        result.status=committing?WithdrawalStatus::partial:WithdrawalStatus::invalid;return result;
    }
}
}
