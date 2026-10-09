#pragma once
#include "movement_view.h"
#include "script_origin.h"
#include "diagnostics/story_reason_snapshot.h"
#include "diagnostics/structural_lifecycle.h"

namespace crml::action_restriction_observer {
// Internal call-local views only. Never publish these addresses to a worker or
// guest, or retain them after the engine callback that owns the inspected data.
struct ActionContext {
    uintptr_t logic{};
    uint64_t entity{};
    uint8_t mode{},flags{};
    uintptr_t status{};
    uint64_t structural_sequence{};
    bool structural_observed{};
};
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
// Deterministic injection at the final native-read boundary, absent in builds.
namespace testing {inline void (*before_status_recheck)(){};}
#endif
inline bool copy_action_context(uintptr_t world,uint16_t player_tag,ActionContext& output) noexcept {
    output={};
    __try {
        using probe::script_origin::detail::read;
        const auto structural_before=structural_lifecycle::snapshot();
        if(structural_before.available && structural_before.active) return false;
        if(!world || player_tag>=16384) return false;
        const auto registry=read<uintptr_t>(world);
        if(!registry || player_tag>=read<uint32_t>(registry+0x50)) return false;
        const auto slots=read<uintptr_t>(registry+0x48);
        if(!slots) return false;
        const auto slot=read<uint16_t>(slots+uint64_t(player_tag)*2);
        if(slot>=16384) return false;
        const auto meta=read<uintptr_t>(world+0x58478),sets=read<uintptr_t>(world+0x58480);
        if(!meta || !sets) return false;
        const auto count=read<uint64_t>(meta+8);
        if(!count || count>8192) return false;
        std::array<unsigned char,1024> membership{};
        const auto membership_size=static_cast<size_t>((count+7)/8);
        const auto bitset=sets+uint64_t(slot)*1024;
        std::memcpy(membership.data(),reinterpret_cast<void*>(bitset),membership_size);
        uintptr_t candidate{};
        uint64_t entity{};
        uint32_t archetype{};
        for(uint32_t i=0;i<count;++i) {
            if(!(membership[i/8]&(1u<<(i%8)))) continue;
            const auto chunk=read<uintptr_t>(world+0x50+uint64_t(i)*8);
            const auto rows=read<uint32_t>(world+0x10448+uint64_t(i)*4);
            if(!rows) continue;
            // Ambiguous player ownership must reject, never select an arbitrary
            // first entity. Current engine player-status bindings pick row zero.
            if(!chunk || rows!=1 || candidate) return false;
            candidate=chunk;archetype=i;entity=read<uint64_t>(chunk+0x10);
        }
        if(!candidate || !entity) return false;
        uintptr_t chunk{};uint32_t row{};
        const auto logic=probe::entity_component(world,entity,0x9988f341,0x810,chunk,row);
        if(!logic || chunk!=candidate || row!=0) return false;
        const auto status=probe::entity_component(world,entity,0x72d38548,2,chunk,row);
        if(!status || chunk!=candidate || row!=0) return false;
        ActionContext result{logic,entity,read<uint8_t>(status),read<uint8_t>(status+1),status};
        if(read<uintptr_t>(world)!=registry || read<uintptr_t>(world+0x58478)!=meta ||
           read<uintptr_t>(registry+0x48)!=slots || player_tag>=read<uint32_t>(registry+0x50) ||
           read<uint16_t>(slots+uint64_t(player_tag)*2)!=slot ||
           read<uintptr_t>(world+0x58480)!=sets || read<uint64_t>(meta+8)!=count ||
           std::memcmp(membership.data(),reinterpret_cast<void*>(bitset),membership_size) ||
           read<uintptr_t>(world+0x50+uint64_t(archetype)*8)!=candidate ||
           read<uint32_t>(world+0x10448+uint64_t(archetype)*4)!=1 ||
           probe::entity_component(world,entity,0x9988f341,0x810,chunk,row)!=logic ||
           chunk!=candidate || row!=0 ||
           probe::entity_component(world,entity,0x72d38548,2,chunk,row)!=status ||
           chunk!=candidate || row!=0) return false;
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
        if(testing::before_status_recheck) testing::before_status_recheck();
#endif
        // Matching endpoints are a diagnostic consistency check, not exclusion
        // of all writers or permission to modify this component. Take the final
        // structural observation after every native dereference in this copy.
        if(read<uint8_t>(status)!=result.mode || read<uint8_t>(status+1)!=result.flags) return false;
        const auto structural_after=structural_lifecycle::snapshot();
        if(structural_before.available || structural_after.available) {
            if(!structural_lifecycle::quiet_interval(structural_before,structural_after)) return false;
            result.structural_observed=true;
            result.structural_sequence=structural_after.sequence;
        }
        output=result;return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
inline uintptr_t fact_dictionary(uintptr_t world) noexcept {
    __try {
        using probe::script_origin::detail::read;
        if(!world) return 0;
        const auto count=read<uint32_t>(world+0x585b8);
        const auto hashes=read<uintptr_t>(world+0x585b0),values=read<uintptr_t>(world+0x585c0);
        if(!hashes || !values || !count || count>65536) return 0;
        uint32_t lo=0,hi=count;
        while(lo<hi) {const auto mid=lo+(hi-lo)/2;
            if(read<uint32_t>(hashes+uint64_t(mid)*4)<0xe7097951) lo=mid+1;else hi=mid;}
        if(lo==count || read<uint32_t>(hashes+uint64_t(lo)*4)!=0xe7097951 ||
           (lo && read<uint32_t>(hashes+uint64_t(lo-1)*4)>=0xe7097951) ||
           (lo+1<count && read<uint32_t>(hashes+uint64_t(lo+1)*4)<=0xe7097951)) return 0;
        const auto dictionary=read<uintptr_t>(values+uint64_t(lo)*8);
        if(!dictionary || read<uint32_t>(world+0x585b8)!=count ||
           read<uintptr_t>(world+0x585b0)!=hashes || read<uintptr_t>(world+0x585c0)!=values ||
           read<uintptr_t>(values+uint64_t(lo)*8)!=dictionary) return 0;
        return dictionary;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return 0;}
}
// Exact build-specific hashes recovered from the engine's string hash routine.
// Diagnostic reads occur during the manager's native binding call; the manager
// receives listener arguments rather than reading these facts there itself.
// This is not proof of scheduler exclusion of concurrent writers;
// matching copies must not be used as permission to mutate engine state.
inline constexpr std::array<uint64_t,6> story_fact_hashes{
    0x58826cf9950ef79d,0xe5cfb331901c9c99,0x2aa60d67b4db3723,
    0xb78360e36660844a,0x1d163817aa56aeee,0x319f2fa94ac62759};
inline bool copy_dictionary_reasons(uintptr_t dictionary,StoryReasons& output) noexcept {
    output={};
    __try {
        using probe::script_origin::detail::read;
        if(!dictionary) return false;
        std::array<unsigned char,0x58> header{},after{};
        std::memcpy(header.data(),reinterpret_cast<void*>(dictionary+0x28),header.size());
        const auto capacity=read<uint64_t>(dictionary+0x48);
        const auto buckets=read<uintptr_t>(dictionary+0x78);
        if(!buckets || !capacity || capacity>(1u<<20) || (capacity&(capacity-1))) return false;
        const auto empty_a=read<uint64_t>(dictionary+0x50),empty_b=read<uint64_t>(dictionary+0x58);
        const auto dead_a=read<uint64_t>(dictionary+0x28),dead_b=read<uint64_t>(dictionary+0x30);
        StoryReasons result{};
        for(size_t i=0;i<story_fact_hashes.size();++i) {
            const auto hash=story_fact_hashes[i];
            auto index=hash&(capacity-1);
            bool found=false;
            // A collision storm rejects the observation instead of making an
            // engine callback walk an unbounded or malformed table.
            for(uint64_t step=0;step<capacity && step<128;++step,index=(index+step)&(capacity-1)) {
                const auto entry=buckets+index*40;
                const auto a=read<uint64_t>(entry),b=read<uint64_t>(entry+8);
                if(a==empty_a && b==empty_b) break;
                if(a==dead_a && b==dead_b) continue;
                if(a || b!=hash) continue;
                // FactDictionary uses its own variant tags: numeric is 1.
                // Lua TValue uses 3 for numbers; that layout is not this table.
                if(read<uint16_t>(entry+0x10)!=1) return false;
                const auto number=read<double>(entry+0x18);
                if(!std::isfinite(number) || number<0 || number>UINT32_MAX || std::floor(number)!=number ||
                   read<uint64_t>(entry)!=a || read<uint64_t>(entry+8)!=b ||
                   read<uint16_t>(entry+0x10)!=1 || read<double>(entry+0x18)!=number) return false;
                result.counts[i]=static_cast<uint32_t>(number);
                if(number) result.active_mask|=uint8_t(1u<<i);
                found=true;break;
            }
            if(!found) return false;
        }
        std::memcpy(after.data(),reinterpret_cast<void*>(dictionary+0x28),after.size());
        if(header!=after) return false;
        result.enabled=result.active_mask!=0;output=result;return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
}
