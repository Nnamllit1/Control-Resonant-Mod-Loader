#include "diagnostics/action_restriction_observer.h"
#include "diagnostics/story_reason_snapshot.h"
#include "diagnostics/action_context_snapshot.h"
#include "diagnostics/applied_story_state.h"
#include "compatibility.h"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ostream>
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>
#endif

namespace crml::action_restriction_observer {
extern "C" void crml_story_writer_entry(void*,uint8_t,uint8_t);
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
extern "C" void crml_story_writer_fixture(void*,uint8_t,uint8_t,void*);
extern "C" const unsigned char crml_story_writer_fixture_return[];
#endif
namespace {
using Conflict=uint8_t(*)(void*,const void*,const void*,const void*,const void*,uint8_t*);
using NativeComparator=uint8_t(*)(void*,const void*,const void*,uint8_t*);
using ActiveMaskQuery=uint8_t(*)(void*,uint64_t);
using StoryBinding=int(*)(void*);
using StoryWriter=void(*)(void*,uint8_t,uint8_t);
constexpr uintptr_t story_binding_rva=0x23c0cf0;
constexpr unsigned char story_prologue[]{0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x40};
// Complete remainder of the binding after RBX receives the VM, including error
// branches. The register provenance must survive other installed code patches.
constexpr unsigned char story_frame_contract[]{0x48,0x8b,0xd9,0x44,0x8b,0xc2,0xe8,0xe6,0x62,0x8d,0x00,0xba,0x01,0x00,0x00,0x00,0x48,0x8b,0xcb,0xe8,0x09,0x81,0x8d,0x00,0x85,0xc0,0x48,0x8b,0x43,0x78,0x40,0x0f,0x95,0xc7,0x48,0x85,0xc0,0x74,0x40,0x44,0x0f,0xb6,0x44,0x24,0x50,0x48,0x8d,0x4c,0x24,0x20,0x48,0x8b,0x10,0xe8,0x17,0xe9,0x01,0x00,0x80,0x7c,0x24,0x38,0x00,0x74,0x30,0x48,0x8b,0x44,0x24,0x30,0x44,0x0f,0xb6,0xc7,0x48,0x8b,0x4c,0x24,0x20,0xb2,0x01,0x48,0x8d,0x0c,0x41,0xe8,0xd7,0xaf,0xeb,0xff,0x48,0x8b,0x5c,0x24,0x58,0x33,0xc0,0x48,0x83,0xc4,0x40,0x5f,0xc3,0x48,0x8b,0xcb,0xe8,0x92,0xf9,0x66,0xff,0xeb,0xb6,0x48,0x8d,0x15,0x99,0xa2,0x75,0x02,0x48,0x8b,0xcb,0xe8,0xe1,0x64,0x8d,0x00,0xcc};
constexpr uintptr_t story_writer_rva=0x227bd30;
constexpr std::array<uintptr_t,2> story_writer_callers{0x23c0d59,0x23c0df9};
constexpr uintptr_t story_writer_tail_jump=0x2742ead,reviewed_image_size=0x6301000;
// The complete reviewed function body [0x227bd30,0x227bd47), ending before INT3 padding.
constexpr unsigned char story_writer_body[]{
    0x0f,0xb6,0x41,0x01,0x45,0x84,0xc0,0x74,0x06,0x0a,0xc2,0x88,0x41,0x01,0xc3,
    0xf6,0xd2,0x22,0xd0,0x88,0x51,0x01,0xc3};
constexpr uintptr_t target_rva=0x22d19d0;
constexpr uintptr_t comparator_rva=0x22d1c10,comparator_return_rva=0x22d201d;
constexpr uintptr_t active_query_rva=0x22d04a0,warning_return_rva=0x1ef3e8c;
constexpr unsigned char comparator_prologue[]{0x40,0x53,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x48,
    0x4d,0x8b,0xf9,0x66,0xc7,0x44,0x24,0x30,0x08,0x10};
constexpr unsigned char comparator_site[]{0x4d,0x8b,0xc5,0x49,0x8b,0xd7,0x49,0x8b,0xcc,
    0xe8,0xf3,0xfb,0xff,0xff,0x84,0xc0,0x0f,0x84,0x8c,0x00,0x00,0x00};
// Complete query body through both returns; the call site includes mask
// selection and the branch that controls warning notification 13.
constexpr unsigned char active_query_body[]{
    0x48,0x8b,0x81,0xd0,0x07,0x00,0x00,0x44,0x8b,0x81,0xd8,0x07,0x00,0x00,0x4e,
    0x8d,0x04,0xc0,0x49,0x3b,0xc0,0x74,0x28,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x4c,0x8b,0x08,0x4f,0x8d,0x14,0x89,0x4a,0x85,0x54,0xd1,0x08,0x74,
    0x08,0x42,0xf6,0x44,0xd1,0x1c,0x04,0x75,0x0c,0x48,0x83,0xc0,0x08,0x49,0x3b,
    0xc0,0x75,0xe1,0x32,0xc0,0xc3,0xb0,0x01,0xc3};
constexpr unsigned char warning_site[]{0x48,0x8b,0xd3,0x48,0x8b,0xcf,0x48,0x83,0xe2,0xfe,0x84,0xc0,0x48,0x0f,0x44,0xd3,0xe8,0x14,0xc6,0x3d,0x00,0x48,0x8b,0x5c,0x24,0x50,0x84,0xc0,0x74,0x0d};
constexpr std::array<uintptr_t,3> caller_rvas{0x22d1b9b,0x22d1d32,0x22d1d51};
// Read from CONTROLResonant.exe SHA-256 9f56611d767c9227a8a1b250c01acf9478b50933c65f86a1e044fcc53df67cb1.
constexpr unsigned char prologue[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20};
constexpr uint64_t capture_ms=600000,story_id=0x04000000,closed=uint64_t{1}<<63;
constexpr uint32_t sample_limit=1024,writer_sample_limit=128,writer_group_limit=writer_sample_limit/4;
constexpr size_t queue_size=256;
constexpr size_t action_bits=50,pair_slots=action_bits*caller_rvas.size()*2;
struct Entry {uint64_t id{};uint8_t state{};};
struct Definition {uint64_t exclusion_mask{};uint8_t policy{};};
struct Pair {Entry left{},right{};Definition left_definition{},right_definition{};uint8_t output{};};
struct Event {
    uint64_t sequence{},occurrence{},time_ms{};
    uintptr_t caller_rva{};
    uint32_t thread{};
    Pair before{},after{};
    uint8_t result{};
    bool returned{},before_valid{},after_valid{},stable{};
    bool reason_event{},reason_valid{};
    StoryReasons reasons{};
    bool writer_event{},writer_before_valid{},writer_after_valid{};
    bool writer_player_context_valid{},writer_player_matches{};
    uint8_t writer_mask{};
    bool writer_enabled{};
    std::array<uint8_t,2> writer_before{},writer_after{};
    bool player_context_valid{},player_matches{},world_matches{},dictionary_matches{},dictionary_values_match{};
    uint8_t player_mode{},player_flags{};
    uint64_t player_entity{},structural_sequence{};
    bool structural_observed{};
    bool writer_manager_valid{},writer_manager_enabled_matches{},writer_context_unchanged{};
    bool applied_reasons_match{};
};
struct Counts {
    std::atomic<uint64_t> calls{},selected{},samples{},dropped{},rejected{},unwinds{},caller_rejected{},throttled{},budget_exhausted{};
} counts;
Conflict original{};
NativeComparator original_comparator{};
ActiveMaskQuery original_active_query{};
StoryBinding original_story{};
StoryWriter original_writer{};
std::atomic<uint64_t> reason_calls{},reason_samples{},reason_rejected{},reason_dropped{},reason_sequence{};
struct WriterCounts {
    std::atomic<uint64_t> calls{},samples{},dropped{},unreadable{},unwinds{},caller_unmapped{},throttled{},budget_exhausted{};
} writer_counts;
std::atomic<uint64_t> writer_sequence{};
uintptr_t image{};
bool attempted{},installed{},writer_installed{},comparator_installed{},warning_installed{}; // Bootstrap worker only; hooks remain pinned.
std::atomic<AllowedActions> tracking_policy{};
std::atomic<RequestPending> tracking_pending{};
std::atomic<bool> tracking_enabled{};
std::atomic<uint64_t> native_skips{};
std::atomic<uint64_t> warning_adjustments{};
void* jump_relay{}; // Pinned for process lifetime after installation.
std::atomic<uint64_t> admission{closed},deadline{},sequence{},occurrence{},capture_started{};
std::array<std::atomic<uint64_t>,pair_slots> next_sample{};
std::atomic<uint32_t> used{};
std::array<std::atomic<uint32_t>,4> writer_used{};
SRWLOCK queue_lock=SRWLOCK_INIT;
std::array<Event,queue_size> queue{};
size_t head{},size{};
AppliedStoryState applied_story;
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
uintptr_t fixture_world{},fixture_other_world{};
#endif

uint16_t player_tag() noexcept {
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
    if(fixture_world) return 0;
#endif
    __try {
        if(!image) return UINT16_MAX;
        return probe::script_origin::detail::read<uint16_t>(compatibility::address(image,0x5c00ca4));
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return UINT16_MAX;
    }
}

uintptr_t current_world() noexcept {
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
    if(fixture_world) return fixture_world;
#endif
    __try {
        using probe::script_origin::detail::read;
        if(!image) return 0;
        // Use the gameplay world selector already mapped for camera/engine
        // observation. The adjacent global-fact singleton can own another world.
        const auto singleton=read<uintptr_t>(compatibility::address(image,0x5c20fe0));
        return singleton?read<uintptr_t>(singleton+8):0;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return 0;}
}
void capture_player(uintptr_t world,const void* expected,Event& event) noexcept {
    __try {
        using probe::script_origin::detail::read;
        ActionContext context{};
        const auto tag=player_tag();
        event.player_context_valid=copy_action_context(world,tag,context);
        if(event.player_context_valid) {
            event.player_matches=context.logic==reinterpret_cast<uintptr_t>(expected);
            event.player_mode=context.mode;event.player_flags=context.flags;
            event.player_entity=context.entity;event.structural_sequence=context.structural_sequence;
            event.structural_observed=context.structural_observed;
        }
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {}
}
uintptr_t capture_writer_player(const void* status,Event& event) noexcept {
    __try {
        using probe::script_origin::detail::read;
        ActionContext context{};
        const auto tag=player_tag();
        const auto world=current_world();
        event.writer_player_context_valid=copy_action_context(world,tag,context);
        if(event.writer_player_context_valid) {
            event.writer_player_matches=context.status==reinterpret_cast<uintptr_t>(status);
            event.player_entity=context.entity;event.structural_sequence=context.structural_sequence;
            event.structural_observed=context.structural_observed;
        }
        return event.writer_player_context_valid?world:0;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {}
    return 0;
}
uintptr_t capture_reason_context(void* state,Event& event) noexcept {
    __try {
        using probe::script_origin::detail::read;
        const auto vm=reinterpret_cast<uintptr_t>(state);
        const auto context=read<uintptr_t>(vm+0x78);
        if(!context) return 0;
        const auto world=read<uintptr_t>(context);
        const auto dictionary=read<uintptr_t>(context+8);
        event.world_matches=world && current_world()==world;
        event.dictionary_matches=dictionary && fact_dictionary(world)==dictionary;
        StoryReasons values{};
        event.dictionary_values_match=event.world_matches && event.dictionary_matches && copy_dictionary_reasons(dictionary,values) &&
            values.counts==event.reasons.counts;
        if(event.world_matches) capture_player(world,nullptr,event);
        return world;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {}
    return 0;
}

bool qualified_context(const void* logic,Identity* qualified,AppliedStoryState::Record* verified=nullptr) noexcept {
    const auto structural_before=structural_lifecycle::snapshot();
    const auto lifecycle_before=player_status_lifecycle::snapshot();
    if(!structural_before.available || structural_before.active ||
       !lifecycle_before.available || lifecycle_before.active) return false;
    const auto world=current_world();
    ActionContext context{};
    __try {
        if(!copy_action_context(world,player_tag(),context) || context.logic!=reinterpret_cast<uintptr_t>(logic) ||
           context.mode!=4 || context.flags!=1) return false;
        StoryReasons reasons{};
        AppliedStoryState::Record record{};
        if(!copy_dictionary_reasons(fact_dictionary(world),reasons) ||
           !applied_story.read(world,context.entity,reasons,structural_before,lifecycle_before,record)) return false;
        ActionContext after{};
        if(current_world()!=world || !copy_action_context(world,player_tag(),after) ||
           after.entity!=context.entity || after.logic!=context.logic || after.status!=context.status ||
           after.mode!=context.mode || after.flags!=context.flags) return false;
        if(!structural_lifecycle::quiet_interval(structural_before,structural_lifecycle::snapshot()) ||
           !player_status_lifecycle::unchanged(lifecycle_before,player_status_lifecycle::snapshot()) ||
           !record.reasons.active_mask || (record.reasons.active_mask&~uint8_t{3}) ||
           !record.reasons.enabled || (!record.reasons.counts[0] && !record.reasons.counts[1]) ||
           context.flags!=1) return false;
        for(size_t i=2;i<record.reasons.counts.size();++i) if(record.reasons.counts[i]) return false;
        if(qualified) *qualified={world,context.entity,structural_before.teardowns};
        if(verified) *verified=record;
        return applied_story.current(record);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool applied_context_matches(const void* logic) noexcept {
    return active() && qualified_context(logic,nullptr);
}

bool read_entry(const void* pointer,Entry& value) noexcept {
    if(!pointer) return false;
    __try {
        const auto* bytes=static_cast<const unsigned char*>(pointer);
        std::memcpy(&value.id,bytes+8,sizeof(value.id));
        value.state=bytes[0x1c];
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool read_definition(const void* pointer,Definition& value) noexcept {
    if(!pointer) return false;
    __try {
        const auto* bytes=static_cast<const unsigned char*>(pointer);
        std::memcpy(&value.exclusion_mask,bytes+0x10,sizeof(value.exclusion_mask));
        value.policy=bytes[0x21];
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool read_output(const uint8_t* pointer,uint8_t& value) noexcept {
    if(!pointer) return false;
    __try {value=*pointer;return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool read_pair(const void* left,const void* left_definition,const void* right,const void* right_definition,
               const uint8_t* output,Pair& value) noexcept {
    const bool a=read_entry(left,value.left);
    const bool b=read_entry(right,value.right);
    const bool c=read_definition(left_definition,value.left_definition);
    const bool d=read_definition(right_definition,value.right_definition);
    const bool e=read_output(output,value.output);
    return a && b && c && d && e;
}
bool same(const Pair& a,const Pair& b) noexcept {
    return a.left.id==b.left.id && a.left.state==b.left.state &&
        a.right.id==b.right.id && a.right.state==b.right.state &&
        a.left_definition.exclusion_mask==b.left_definition.exclusion_mask &&
        a.left_definition.policy==b.left_definition.policy &&
        a.right_definition.exclusion_mask==b.right_definition.exclusion_mask &&
        a.right_definition.policy==b.right_definition.policy;
}
bool caller_matches(uintptr_t caller,uintptr_t base) noexcept {
    if(!base) return false;
    for(const auto rva:caller_rvas) if(caller==base+rva) return true;
    return false;
}
bool writer_caller_matches(uintptr_t caller,uintptr_t base) noexcept {
    if(!base) return false;
    for(const auto rva:story_writer_callers) if(caller==base+rva) return true;
    return false;
}
uint32_t writer_caller_rva(uintptr_t caller,uintptr_t base) noexcept {
    return base && caller>=base && caller-base<reviewed_image_size?
        static_cast<uint32_t>(caller-base):UINT32_MAX;
}
bool read_writer_bytes(const void* pointer,std::array<uint8_t,2>& value) noexcept {
    if(!pointer) return false;
    __try {std::memcpy(value.data(),pointer,value.size());return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
size_t pair_slot(uintptr_t caller,uint64_t left,uint64_t right) noexcept {
    const bool story_left=left==story_id;
    const uint64_t action=story_left?right:left;
    if((story_left?right==story_id:left==story_id) || !action || action>=(uint64_t{1}<<action_bits) ||
       (action&(action-1))) return pair_slots;
    size_t bit{};
    for(auto remaining=action;remaining>1;remaining>>=1) ++bit;
    for(size_t i=0;i<caller_rvas.size();++i)
        if(caller==image+caller_rvas[i]) return (i*2+size_t(!story_left))*action_bits+bit;
    return pair_slots;
}
bool admit(uint64_t now) noexcept {
    const auto until=deadline.load(std::memory_order_acquire);
    if(!until || now>=until) return false;
    auto state=admission.load(std::memory_order_acquire);
    while(!(state&closed) && state!=closed-1)
        if(admission.compare_exchange_weak(state,state+1,std::memory_order_acquire)) return true;
    return false;
}
bool sample_now(uint64_t now,size_t slot) noexcept {
    if(used.load(std::memory_order_relaxed)>=sample_limit) {++counts.budget_exhausted;return false;}
    auto& next=next_sample[slot];
    auto previous=next.load(std::memory_order_relaxed);
    if(now<previous || !next.compare_exchange_strong(previous,now+250,std::memory_order_relaxed)) {
        ++counts.throttled;return false;
    }
    auto count=used.load(std::memory_order_relaxed);
    while(count<sample_limit)
        if(used.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) return true;
    ++counts.budget_exhausted;return false;
}
size_t writer_group(uint8_t mask) noexcept {return mask==1?0:mask==2?1:mask==4?2:3;}
bool sample_writer(uint8_t mask,uint64_t now) noexcept {
    // Reserve most of each category for later gameplay. A busy startup must not
    // spend all 32 observations before the player reaches a restricted area.
    const auto started=capture_started.load(std::memory_order_relaxed);
    const auto elapsed=now>=started?now-started:0;
    const auto allowance=static_cast<uint32_t>((std::min)(uint64_t{writer_group_limit},8+elapsed/20000));
    auto& used_for_group=writer_used[writer_group(mask)];
    auto count=used_for_group.load(std::memory_order_relaxed);
    while(count<allowance)
        if(used_for_group.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) return true;
    if(count>=writer_group_limit) ++writer_counts.budget_exhausted;
    else ++writer_counts.throttled;
    return false;
}
void queue_writer(Event& event) noexcept {
    if(!TryAcquireSRWLockExclusive(&queue_lock)) {++writer_counts.dropped;return;}
    if(size==queue_size) ++writer_counts.dropped;
    else {queue[(head+size)%queue_size]=event;++size;}
    ReleaseSRWLockExclusive(&queue_lock);
}
void finish_writer(Event& event,bool sampled,bool admitted,const void* status,
                   uintptr_t writer_world,const structural_lifecycle::Snapshot& before,
                   uint64_t write_token,const player_status_lifecycle::Snapshot& lifecycle_before) noexcept {
    if(admitted && !event.returned) ++writer_counts.unwinds;
    if(sampled || write_token) {
        if(event.returned) event.writer_after_valid=read_writer_bytes(status,event.writer_after);
        if(event.returned && event.writer_manager_valid) {
            Event after{};const auto after_world=capture_writer_player(status,after);
            event.writer_context_unchanged=event.writer_player_context_valid && event.writer_player_matches &&
                after.writer_player_context_valid && after.writer_player_matches &&
                writer_world && writer_world==after_world &&
                event.player_entity==after.player_entity &&
                structural_lifecycle::quiet_interval(before,structural_lifecycle::snapshot());
        }
        if(admitted && (!event.writer_before_valid || (event.returned && !event.writer_after_valid)))
            ++writer_counts.unreadable;
        if(write_token) {
            const auto structural_after=structural_lifecycle::snapshot();
            const auto lifecycle_after=player_status_lifecycle::snapshot();
            const auto expected=event.writer_enabled?uint8_t(event.writer_before[1]|1):uint8_t(event.writer_before[1]&~1);
            const bool valid=event.returned && event.writer_manager_valid && event.writer_manager_enabled_matches &&
                event.world_matches && event.dictionary_matches && event.dictionary_values_match &&
                event.writer_context_unchanged && event.writer_before_valid && event.writer_after_valid &&
                event.writer_before[0]==event.writer_after[0] && event.writer_after[1]==expected &&
                structural_lifecycle::quiet_interval(before,structural_after) &&
                player_status_lifecycle::unchanged(lifecycle_before,lifecycle_after);
            AppliedStoryState::Record record{writer_world,event.player_entity,before.teardowns,before.unwinds,
                lifecycle_before.sequence,0,event.reasons};
            applied_story.end_write(write_token,valid?&record:nullptr);
        }
        if(sampled) queue_writer(event);
    }
    if(admitted) admission.fetch_sub(1,std::memory_order_release);
}
void forward_writer(void* status,uint8_t mask,uint8_t enabled,bool admitted,uintptr_t caller,uint64_t now,void* binding_vm=nullptr) {
    Event event{};
    structural_lifecycle::Snapshot structural_before{};
    uintptr_t writer_world{}; // Call-local identity only; never queued or logged.
    const auto write_token=(admitted || tracking_enabled.load(std::memory_order_acquire)) && (mask&1)?applied_story.begin_write():0;
    const auto lifecycle_before=write_token?player_status_lifecycle::snapshot():player_status_lifecycle::Snapshot{};
    bool sampled=false;
    if(admitted || write_token) {
        if(admitted) {
            ++writer_counts.calls;
            if(!writer_caller_matches(caller,image)) ++writer_counts.caller_unmapped;
            sampled=sample_writer(mask,now);
        }
        if(sampled || write_token) {
            structural_before=structural_lifecycle::snapshot();
            if(sampled) ++writer_counts.samples;
            event.writer_event=true;
            if(sampled) {
                event.sequence=writer_sequence.fetch_add(1,std::memory_order_relaxed)+1;
                event.occurrence=occurrence.fetch_add(1,std::memory_order_relaxed)+1;
            }
            event.time_ms=now;event.thread=GetCurrentThreadId();event.caller_rva=writer_caller_rva(caller,image);
            event.writer_mask=mask;event.writer_enabled=enabled!=0;
            event.writer_before_valid=read_writer_bytes(status,event.writer_before);
            writer_world=capture_writer_player(status,event);
            // At this exact call, binding RBX still holds the VM. Read it only
            // after all caller/profile guards, and retain copied values only.
            // No thread-local scope spans the binding's Lua error paths.
            if(mask==1 && caller==image+story_writer_callers[0] && binding_vm) {
                event.writer_manager_valid=copy_story_reasons(binding_vm,event.reasons);
                if(event.writer_manager_valid) {
                    event.writer_manager_enabled_matches=event.reasons.enabled==(enabled!=0);
                    const auto manager_world=capture_reason_context(binding_vm,event);
                    event.world_matches=event.world_matches && writer_world && manager_world==writer_world;
                }
            }
        }
    }
    __try {
        original_writer(status,mask,enabled);
        event.returned=true;
    } __finally {
        if(admitted || write_token) finish_writer(event,sampled,admitted,status,writer_world,structural_before,write_token,lifecycle_before);
    }
}
void writer_dispatch(void* status,uint8_t mask,uint8_t enabled,void* vm,uintptr_t caller) {
    const auto now=GetTickCount64();
    const bool admitted=admit(now);
    if(!admitted && !tracking_enabled.load(std::memory_order_acquire)) {
        if(mask&1) applied_story.invalidate();
        original_writer(status,mask,enabled);return;
    }
    forward_writer(status,mask,enabled,admitted,caller,now,vm);
}
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
void writer_hook(void* status,uint8_t mask,uint8_t enabled) {
    writer_dispatch(status,mask,enabled,nullptr,reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
#endif
void finish(Event& event,bool sampled,const void* left,const void* left_definition,
            const void* right,const void* right_definition,const uint8_t* output) noexcept {
    if(!event.returned) ++counts.unwinds;
    if(sampled) {
        if(event.returned) {
            event.after_valid=read_pair(left,left_definition,right,right_definition,output,event.after);
            event.stable=event.before_valid && event.after_valid && same(event.before,event.after);
            if(!event.before_valid || !event.after_valid || !event.stable) ++counts.rejected;
        }
        if(!TryAcquireSRWLockExclusive(&queue_lock)) ++counts.dropped;
        else {
            if(size==queue_size) ++counts.dropped;
            else {queue[(head+size)%queue_size]=event;++size;}
            ReleaseSRWLockExclusive(&queue_lock);
        }
    }
    admission.fetch_sub(1,std::memory_order_release);
}
uint8_t forward(void* logic,const void* left,const void* left_definition,const void* right,
                const void* right_definition,uint8_t* output,bool admitted,uintptr_t caller,uint64_t now) {
    Event event{};
    bool sampled=false;
    uint8_t result{};
    if(admitted) {
        ++counts.calls;
        if(!caller_matches(caller,image)) ++counts.caller_rejected;
        else {
            Entry a{},b{};
            if(!read_entry(left,a) || !read_entry(right,b)) ++counts.rejected;
            else if(a.id==story_id || b.id==story_id) {
                ++counts.selected;
                const auto slot=pair_slot(caller,a.id,b.id);
                if(slot==pair_slots) ++counts.rejected;
                else if(sample_now(now,slot)) {
                    sampled=true;++counts.samples;
                    event.sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
                    event.occurrence=occurrence.fetch_add(1,std::memory_order_relaxed)+1;
                    event.time_ms=now;event.thread=GetCurrentThreadId();event.caller_rva=caller-image;
                    event.before_valid=read_pair(left,left_definition,right,right_definition,output,event.before);
                    capture_player(current_world(),logic,event);
                    event.applied_reasons_match=applied_context_matches(logic);
                }
            }
        }
    }
    // The engine may unwind. The finally block releases admission without swallowing it.
    __try {
        result=original(logic,left,left_definition,right,right_definition,output);
        event.result=result;event.returned=true;
    } __finally {
        if(admitted) finish(event,sampled,left,left_definition,right,right_definition,output);
    }
    return result;
}
uint8_t hook(void* logic,const void* left,const void* left_definition,const void* right,
             const void* right_definition,uint8_t* output) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto now=GetTickCount64();
    if(!admit(now)) return original(logic,left,left_definition,right,right_definition,output);
    return forward(logic,left,left_definition,right,right_definition,output,true,caller,now);
}
bool indexed_entry(uintptr_t base,const void* entry,uint64_t id) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(entry);
    if(!base || !id || (id&(id-1)) || address<base) return false;
    size_t index{};
    for(auto remaining=id;remaining>1;remaining>>=1) ++index;
    return index<50 && address-base==index*0x28;
}
constexpr uint64_t supported_actions=1|2|0x10|0x20|0x10000;
bool stable_after_policy(void* logic,const Identity& who,const AppliedStoryState::Record& record,
                         const void* requested=nullptr,const void* interferer=nullptr,
                         uint64_t action_id=0) noexcept {
    // Re-resolve after host policy and every native read. The writer generation
    // is the final permission check for both the action and warning decisions.
    const auto structural_before=structural_lifecycle::snapshot();
    const auto lifecycle_before=player_status_lifecycle::snapshot();
    if(!structural_before.available || structural_before.active ||
       structural_before.teardowns!=who.lifetime ||
       structural_before.unwinds!=record.structural_unwinds ||
       !lifecycle_before.available || lifecycle_before.active ||
       lifecycle_before.sequence!=record.status_sequence) return false;
    ActionContext after{};Entry later_action{},later_candidate{};
    if(current_world()!=who.world || !copy_action_context(who.world,player_tag(),after) ||
       after.logic!=reinterpret_cast<uintptr_t>(logic) || after.entity!=who.entity ||
       after.mode!=4 || after.flags!=1 ||
       (requested && (!read_entry(requested,later_action) || later_action.id!=action_id)) ||
       (interferer && (!read_entry(interferer,later_candidate) || later_candidate.id!=story_id)) ||
       !structural_lifecycle::quiet_interval(structural_before,structural_lifecycle::snapshot()) ||
       !player_status_lifecycle::unchanged(lifecycle_before,player_status_lifecycle::snapshot()) ||
       !tracking_enabled.load(std::memory_order_acquire)) return false;
    return applied_story.current(record);
}
bool native_exception(void* logic,const void* requested,const void* interferer,
                      uintptr_t caller,uint64_t now) noexcept {
    if(!tracking_enabled.load(std::memory_order_acquire) || !image ||
       caller!=image+comparator_return_rva) return false;
    // This exact caller has requested on the left and an active candidate on
    // the right. Reject both IDs before resolving player state or owner policy.
    Entry candidate{},action{};
    if(!read_entry(interferer,candidate) || candidate.id!=story_id ||
       !read_entry(requested,action)) return false;
    if(!(action.id&supported_actions) || (action.id&(action.id-1))) return false;
    const auto pending=tracking_pending.load(std::memory_order_acquire);
    if(pending && !pending(now)) return false;
    Identity who{};AppliedStoryState::Record record{};
    if(!qualified_context(logic,&who,&record) ||
       !indexed_entry(reinterpret_cast<uintptr_t>(logic),requested,action.id) ||
       !indexed_entry(reinterpret_cast<uintptr_t>(logic),interferer,story_id) ||
       requested==interferer) return false;
    const auto policy=tracking_policy.load(std::memory_order_acquire);
    if(!policy || !(policy(who,now)&action.id)) return false;
    if(!stable_after_policy(logic,who,record,requested,interferer,action.id)) return false;
    native_skips.fetch_add(1,std::memory_order_relaxed);
    return true;
}
uint8_t comparator_hook(void* logic,const void* requested,const void* interferer,uint8_t* output) {
    if(native_exception(logic,requested,interferer,reinterpret_cast<uintptr_t>(_ReturnAddress()),GetTickCount64()))
        return 0;
    return original_comparator(logic,requested,interferer,output);
}
uint8_t warning_dispatch(void* logic,uint64_t mask,uintptr_t caller,uint64_t now) {
    if(!tracking_enabled.load(std::memory_order_acquire) || !image ||
       caller!=image+warning_return_rva || !(mask&supported_actions))
        return original_active_query(logic,mask);
    const auto pending=tracking_pending.load(std::memory_order_acquire);
    if(pending && !pending(now)) return original_active_query(logic,mask);
    Identity who{};AppliedStoryState::Record record{};
    if(!qualified_context(logic,&who,&record)) return original_active_query(logic,mask);
    const auto policy=tracking_policy.load(std::memory_order_acquire);
    const auto allowed=policy?mask&supported_actions&policy(who,now):0;
    if(!allowed || !stable_after_policy(logic,who,record)) return original_active_query(logic,mask);
    warning_adjustments.fetch_add(1,std::memory_order_relaxed);
    return original_active_query(logic,mask&~allowed);
}
uint8_t warning_hook(void* logic,uint64_t mask) {
    return warning_dispatch(logic,mask,reinterpret_cast<uintptr_t>(_ReturnAddress()),GetTickCount64());
}
void observe_story_call(void* vm,uint64_t now) noexcept {
    if(!admit(now)) return;
    const auto ordinal=reason_calls.fetch_add(1,std::memory_order_relaxed);
    if(ordinal<128) {
        Event event{};event.reason_event=true;
        event.sequence=reason_sequence.fetch_add(1,std::memory_order_relaxed)+1;
        event.occurrence=occurrence.fetch_add(1,std::memory_order_relaxed)+1;
        event.time_ms=now;event.thread=GetCurrentThreadId();
        event.reason_valid=copy_story_reasons(vm,event.reasons);
        if(event.reason_valid) capture_reason_context(vm,event);
        ++reason_samples;
        if(!event.reason_valid) ++reason_rejected;
        if(!TryAcquireSRWLockExclusive(&queue_lock)) ++reason_dropped;
        else {
            if(size==queue_size) ++reason_dropped;
            else {queue[(head+size)%queue_size]=event;++size;}
            ReleaseSRWLockExclusive(&queue_lock);
        }
    }
    admission.fetch_sub(1,std::memory_order_release);
}
int story_hook(void* vm) {
    observe_story_call(vm,GetTickCount64());
    // All diagnostic work is finished before entering the VM binding: even a
    // Lua error/nonlocal return cannot strand admission or retain stack memory.
    return original_story(vm);
}
bool supported() noexcept {
    return compatibility::reviewed_build &&
        compatibility::engine_profile==compatibility::EngineProfile::october_patch &&
        compatibility::patch_sha==std::string_view("9f56611d767c9227a8a1b250c01acf9478b50933c65f86a1e044fcc53df67cb1");
}
bool relative_transfer_matches(uintptr_t site,uintptr_t target,unsigned char opcode) noexcept {
    const auto delta=static_cast<int32_t>(target-(site+5));
    return compatibility::matches(reinterpret_cast<void*>(site),&opcode,1) &&
        compatibility::matches(reinterpret_cast<void*>(site+1),&delta,sizeof(delta));
}
bool call_matches(uintptr_t call,uintptr_t target) noexcept {
    return relative_transfer_matches(call,target,0xe8);
}
void event_json(std::ostream& out,const Event& e) {
    const auto flags=out.flags();out<<std::dec<<std::noshowpos<<std::noboolalpha;
    if(e.writer_event) {
        out<<"Capability story_writer: {\"schema\":1,\"type\":\"sample\",\"status\":\""
           <<(!e.returned?"unwind":!e.writer_before_valid || !e.writer_after_valid?"unreadable":"ok")
           <<"\",\"sequence\":"<<e.sequence<<",\"occurrence\":"<<e.occurrence<<",\"time_ms\":"<<e.time_ms
           <<",\"thread\":"<<e.thread<<",\"caller_rva\":\""<<e.caller_rva
           <<"\",\"returned\":"<<(e.returned?"true":"false")
           <<",\"bytes_valid\":"<<(e.writer_before_valid && e.writer_after_valid?"true":"false")
           <<",\"writer_player_context_valid\":"<<(e.writer_player_context_valid?"true":"false")
           <<",\"writer_player_matches\":"<<(e.writer_player_matches?"true":"false")
           <<",\"player_entity\":"<<e.player_entity
           <<",\"structural_observed\":"<<(e.structural_observed?"true":"false")
           <<",\"structural_sequence\":"<<e.structural_sequence
           <<",\"mask\":"<<unsigned(e.writer_mask)
           <<",\"enabled\":"<<(e.writer_enabled?"true":"false")
           <<",\"before_byte0\":"<<unsigned(e.writer_before[0])
           <<",\"after_byte0\":"<<unsigned(e.writer_after[0])
           <<",\"before_flags\":"<<unsigned(e.writer_before[1])
           <<",\"after_flags\":"<<unsigned(e.writer_after[1])
           <<",\"manager_reasons_valid\":"<<(e.writer_manager_valid?"true":"false");
        if(e.writer_manager_valid) {
            out<<",\"manager_enabled_matches\":"<<(e.writer_manager_enabled_matches?"true":"false")
               <<",\"manager_world_matches\":"<<(e.world_matches?"true":"false")
               <<",\"writer_context_unchanged\":"<<(e.writer_context_unchanged?"true":"false")
               <<",\"manager_active_mask\":"<<unsigned(e.reasons.active_mask)<<",\"manager_counts\":[";
            for(size_t i=0;i<e.reasons.counts.size();++i) {if(i) out<<',';out<<e.reasons.counts[i];}
            out<<']';
        }
        out<<"}\n";
        out.flags(flags);return;
    }
    if(e.reason_event) {
        out<<"Capability story_reason: {\"schema\":1,\"type\":\"sample\",\"status\":\""
           <<(e.reason_valid?"selected":"unrecognized")<<"\",\"sequence\":"<<e.sequence
           <<",\"occurrence\":"<<e.occurrence
           <<",\"time_ms\":"<<e.time_ms<<",\"thread\":"<<e.thread<<",\"phase\":\"binding_request\"";
        if(e.reason_valid) {
            out<<",\"enabled\":"<<(e.reasons.enabled?"true":"false")
               <<",\"active_mask\":"<<unsigned(e.reasons.active_mask)<<",\"counts\":[";
            for(size_t i=0;i<e.reasons.counts.size();++i) {if(i) out<<',';out<<e.reasons.counts[i];}
            out<<']'<<",\"world_matches\":"<<(e.world_matches?"true":"false")
               <<",\"dictionary_matches\":"<<(e.dictionary_matches?"true":"false")
               <<",\"dictionary_values_match\":"<<(e.dictionary_values_match?"true":"false")
               <<",\"player_context_valid\":"<<(e.player_context_valid?"true":"false")
               <<",\"player_entity\":"<<e.player_entity
               <<",\"structural_observed\":"<<(e.structural_observed?"true":"false")
               <<",\"structural_sequence\":"<<e.structural_sequence;
        }
        out<<"}\n";out.flags(flags);return;
    }
    out<<"Capability restriction: {\"schema\":1,\"type\":\"sample\",\"status\":\""
       <<(!e.returned?"unwind":!e.before_valid || !e.after_valid?"unreadable":!e.stable?"mutated":"ok")
       <<"\",\"sequence\":"<<e.sequence<<",\"occurrence\":"<<e.occurrence<<",\"time_ms\":"<<e.time_ms
       <<",\"thread\":"<<e.thread<<",\"caller_rva\":\""<<e.caller_rva
       <<"\",\"left_id\":\""<<e.before.left.id<<"\",\"right_id\":\""<<e.before.right.id
       <<"\",\"left_state\":"<<unsigned(e.before.left.state)<<",\"right_state\":"<<unsigned(e.before.right.state)
       <<",\"left_exclusion_mask\":\""<<e.before.left_definition.exclusion_mask
       <<"\",\"right_exclusion_mask\":\""<<e.before.right_definition.exclusion_mask
       <<"\",\"left_policy\":"<<unsigned(e.before.left_definition.policy)
       <<",\"right_policy\":"<<unsigned(e.before.right_definition.policy)
       <<",\"outbyte_before\":"<<unsigned(e.before.output)<<",\"outbyte_after\":"<<unsigned(e.after.output)
       <<",\"result\":"<<unsigned(e.result)<<",\"conflict\":"<<(e.result?"true":"false")
       <<",\"before_valid\":"<<(e.before_valid?"true":"false")
       <<",\"after_valid\":"<<(e.after_valid?"true":"false")
       <<",\"stable\":"<<(e.stable?"true":"false")
       <<",\"player_context_valid\":"<<(e.player_context_valid?"true":"false")
       <<",\"player_matches\":"<<(e.player_matches?"true":"false")
       <<",\"player_entity\":"<<e.player_entity
       <<",\"structural_observed\":"<<(e.structural_observed?"true":"false")
       <<",\"structural_sequence\":"<<e.structural_sequence
       <<",\"player_mode\":"<<unsigned(e.player_mode)<<",\"player_flags\":"<<unsigned(e.player_flags)
       <<",\"applied_reasons_match\":"<<(e.applied_reasons_match?"true":"false")<<"}\n";
    out.flags(flags);
}
void totals_json(std::ostream& out,uint64_t now,bool final) {
    const auto flags=out.flags();out<<std::dec<<std::noshowpos<<std::noboolalpha;
    const auto pending=admission.load(std::memory_order_acquire)&~closed;
    out<<"Capability restriction: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(final?"complete":active()?"active":"draining")<<"\",\"time_ms\":"<<now
       <<",\"capture_ms\":"<<capture_ms<<",\"calls\":"<<counts.calls.load()
       <<",\"selected\":"<<counts.selected.load()<<",\"samples\":"<<counts.samples.load()
       <<",\"dropped\":"<<counts.dropped.load()<<",\"rejected\":"<<counts.rejected.load()
       <<",\"unwinds\":"<<counts.unwinds.load()<<",\"caller_rejected\":"<<counts.caller_rejected.load()
       <<",\"throttled\":"<<counts.throttled.load()<<",\"budget_exhausted\":"<<counts.budget_exhausted.load()
       <<",\"in_flight\":"<<pending<<",\"pending\":"<<pending<<"}\n";
    out<<"Capability story_reason: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(final?"complete":active()?"active":"draining")<<"\",\"time_ms\":"<<now
       <<",\"calls\":"<<reason_calls.load()<<",\"samples\":"<<reason_samples.load()
       <<",\"rejected\":"<<reason_rejected.load()<<",\"dropped\":"<<reason_dropped.load()
       <<",\"budget_exhausted\":"<<(reason_calls.load()>128?reason_calls.load()-128:0)
       <<",\"pending\":"<<pending<<"}\n";
    out<<"Capability story_writer: {\"schema\":1,\"type\":\"totals\",\"status\":\""
       <<(!writer_installed?"unavailable":final?"complete":active()?"active":"draining")
       <<"\",\"time_ms\":"<<now<<",\"capture_ms\":"<<capture_ms
       <<",\"calls\":"<<writer_counts.calls.load()<<",\"samples\":"<<writer_counts.samples.load()
       <<",\"dropped\":"<<writer_counts.dropped.load()<<",\"unreadable\":"<<writer_counts.unreadable.load()
       <<",\"unwinds\":"<<writer_counts.unwinds.load()<<",\"caller_unmapped\":"<<writer_counts.caller_unmapped.load()
       <<",\"throttled\":"<<writer_counts.throttled.load()
       <<",\"budget_exhausted\":"<<writer_counts.budget_exhausted.load()
       <<",\"pending\":"<<pending<<"}\n";
    out.flags(flags);
}
bool install_hooks() noexcept {
    if(attempted) return installed;
    if(!supported()) return false;
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!image || !compatibility::matches(reinterpret_cast<void*>(image+target_rva),prologue,sizeof(prologue)) ||
       !compatibility::matches(reinterpret_cast<void*>(image+story_binding_rva),story_prologue,sizeof(story_prologue))) return false;
    for(const auto caller:caller_rvas) if(!call_matches(image+caller-5,image+target_rva)) return false;
    bool writer_verified=compatibility::matches(reinterpret_cast<void*>(image+story_writer_rva),
                                                story_writer_body,sizeof(story_writer_body));
    writer_verified=writer_verified && compatibility::matches(reinterpret_cast<void*>(image+0x23c0cff),
                                                               story_frame_contract,sizeof(story_frame_contract));
    for(const auto caller:story_writer_callers)
        writer_verified=writer_verified && call_matches(image+caller-5,image+story_writer_rva);
    writer_verified=writer_verified && relative_transfer_matches(image+story_writer_tail_jump,
                                                                  image+story_writer_rva,0xe9);
    const bool comparator_verified=compatibility::matches(reinterpret_cast<void*>(image+comparator_rva),
                            comparator_prologue,sizeof(comparator_prologue)) &&
        compatibility::matches(reinterpret_cast<void*>(image+0x22d200f),comparator_site,sizeof(comparator_site)) &&
        call_matches(image+comparator_return_rva-5,image+comparator_rva);
    const bool warning_verified=compatibility::matches(reinterpret_cast<void*>(image+active_query_rva),
                            active_query_body,sizeof(active_query_body)) &&
        compatibility::matches(reinterpret_cast<void*>(image+0x1ef3e77),warning_site,sizeof(warning_site)) &&
        call_matches(image+warning_return_rva-5,image+active_query_rva);
    attempted=true;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    const auto target=reinterpret_cast<void*>(image+target_rva);
    if(MH_CreateHook(target,reinterpret_cast<void*>(&hook),reinterpret_cast<void**>(&original))!=MH_OK) return false;
    const auto story_target=reinterpret_cast<void*>(image+story_binding_rva);
    if(MH_CreateHook(story_target,reinterpret_cast<void*>(&story_hook),reinterpret_cast<void**>(&original_story))!=MH_OK) {
        MH_RemoveHook(target);return false;
    }
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(story_target);MH_RemoveHook(target);return false;}
    // Every enabled hook stays pinned in passthrough on a partial failure.
    if(MH_EnableHook(story_target)!=MH_OK) {MH_RemoveHook(story_target);return false;}
    if(writer_verified) {
        const auto writer_target=reinterpret_cast<void*>(image+story_writer_rva);
        if(MH_CreateHook(writer_target,reinterpret_cast<void*>(&crml_story_writer_entry),
                         reinterpret_cast<void**>(&original_writer))==MH_OK) {
            if(MH_EnableHook(writer_target)==MH_OK) writer_installed=true;
            else MH_RemoveHook(writer_target);
        }
    }
    if(comparator_verified) {
        const auto compare_target=reinterpret_cast<void*>(image+comparator_rva);
        if(MH_CreateHook(compare_target,reinterpret_cast<void*>(&comparator_hook),
                         reinterpret_cast<void**>(&original_comparator))==MH_OK) {
            if(MH_EnableHook(compare_target)==MH_OK) comparator_installed=true;
            else MH_RemoveHook(compare_target);
        }
    }
    if(warning_verified) {
        const auto query_target=reinterpret_cast<void*>(image+active_query_rva);
        if(MH_CreateHook(query_target,reinterpret_cast<void*>(&warning_hook),
                         reinterpret_cast<void**>(&original_active_query))==MH_OK) {
            if(MH_EnableHook(query_target)==MH_OK) warning_installed=true;
            else MH_RemoveHook(query_target);
        }
    }
    installed=true;
    return true;
}
}
extern "C" void crml_story_writer_bridge(void* status,uint8_t mask,uint8_t enabled,void* vm,uintptr_t caller) {
    writer_dispatch(status,mask,enabled,vm,caller);
}

bool start() noexcept {
    if(!install_hooks() || active()) return false;
    if(!structural_lifecycle::snapshot().available && !structural_lifecycle::start()) return false;
    if(!player_status_lifecycle::snapshot().available && !player_status_lifecycle::start()) return false;
    const auto now=GetTickCount64();capture_started.store(now,std::memory_order_relaxed);
    admission.store(0,std::memory_order_release);
    deadline.store(now+capture_ms,std::memory_order_release);
    return true;
}
bool active() noexcept {const auto until=deadline.load(std::memory_order_acquire);return until && GetTickCount64()<until;}
void stop() noexcept {
    deadline.store(0,std::memory_order_release);admission.fetch_or(closed,std::memory_order_acq_rel);
    if(!tracking_enabled.load(std::memory_order_acquire)) {
        applied_story.invalidate();
        structural_lifecycle::stop();
        player_status_lifecycle::stop();
    }
}
bool start_tracking(AllowedActions policy,RequestPending pending) noexcept {
    if(!policy || !install_hooks() || !writer_installed || !comparator_installed || !warning_installed) return false;
    if(!structural_lifecycle::snapshot().available && !structural_lifecycle::start()) return false;
    if(!player_status_lifecycle::snapshot().available && !player_status_lifecycle::start()) return false;
    applied_story.invalidate();
    tracking_policy.store(policy,std::memory_order_release);
    tracking_pending.store(pending,std::memory_order_release);
    tracking_enabled.store(true,std::memory_order_release);
    return true;
}
void stop_tracking() noexcept {
    tracking_enabled.store(false,std::memory_order_release);
    tracking_pending.store(nullptr,std::memory_order_release);
    tracking_policy.store(nullptr,std::memory_order_release);
    applied_story.invalidate();
    if(!active()) {
        structural_lifecycle::stop();
        player_status_lifecycle::stop();
    }
}
bool identity(Identity& out) noexcept {
    out={};
    if(!tracking_enabled.load(std::memory_order_acquire)) return false;
    const auto structural_before=structural_lifecycle::snapshot();
    const auto lifecycle_before=player_status_lifecycle::snapshot();
    if(!structural_before.available || structural_before.active ||
       !lifecycle_before.available || lifecycle_before.active) return false;
    const auto world=current_world();ActionContext context{},after{};
    if(!copy_action_context(world,player_tag(),context) || !context.entity ||
       current_world()!=world || !copy_action_context(world,player_tag(),after) ||
       after.entity!=context.entity || after.logic!=context.logic || after.status!=context.status ||
       after.mode!=context.mode || after.flags!=context.flags ||
       !structural_lifecycle::quiet_interval(structural_before,structural_lifecycle::snapshot()) ||
       !player_status_lifecycle::unchanged(lifecycle_before,player_status_lifecycle::snapshot()) ||
       !tracking_enabled.load(std::memory_order_acquire)) return false;
    out={world,context.entity,structural_before.teardowns};return true;
}
uint64_t skipped() noexcept {return native_skips.load(std::memory_order_relaxed);}
uint64_t warning_adjusted() noexcept {return warning_adjustments.load(std::memory_order_relaxed);}
void poll(std::ostream& out) {
    static uint64_t last{};static bool finished{};
    if(!installed || finished) return;
    const auto now=GetTickCount64();
    if(!active()) stop();
    const bool final=!active() && !(admission.load(std::memory_order_acquire)&~closed) &&
                     !structural_lifecycle::snapshot().active && !player_status_lifecycle::snapshot().active;
    if(last && now-last<1000 && !final) return;
    last=now;
    Event batch[32]{};
    for(size_t b=0;b<queue_size/32;++b) {
        size_t taken{};
        AcquireSRWLockExclusive(&queue_lock);
        while(taken<std::size(batch) && size) {batch[taken++]=queue[head];head=(head+1)%queue_size;--size;}
        ReleaseSRWLockExclusive(&queue_lock);
        for(size_t i=0;i<taken;++i) event_json(out,batch[i]);
        if(taken<std::size(batch)) break;
    }
    totals_json(out,now,final);
    structural_lifecycle::report(out);
    player_status_lifecycle::report(out);
    if(final) finished=true;
    out.flush();
}
#ifdef CRML_ACTION_RESTRICTION_OBSERVER_TESTING
namespace testing {
void context_fixture(uintptr_t world,uintptr_t other) noexcept {fixture_world=world;fixture_other_world=other;}
namespace {
constexpr uintptr_t test_base=0x140000000;
constexpr DWORD test_exception=0xe043524d;
std::array<const void*,6> seen{};
unsigned calls{};
bool fail{};
uint8_t stub(void* logic,const void* left,const void* ld,const void* right,const void* rd,uint8_t* output) {
    seen={logic,left,ld,right,rd,output};++calls;
    if(fail) RaiseException(test_exception,0,0,nullptr);
    if(output) *output=17;
    return 3;
}
bool catch_unwind(void* logic,const void* left,const void* ld,const void* right,const void* rd,uint8_t* output) {
    __try {const auto now=GetTickCount64();forward(logic,left,ld,right,rd,output,admit(now),test_base+caller_rvas[0],now);}
    __except(GetExceptionCode()==test_exception?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
template<size_t N,class T> void put(std::array<unsigned char,N>& a,size_t at,T value) {std::memcpy(a.data()+at,&value,sizeof(value));}
}
bool callers() noexcept {
    for(const auto rva:caller_rvas) if(!caller_matches(test_base+rva,test_base)) return false;
    return !caller_matches(test_base+caller_rvas[0]+1,test_base) && !caller_matches(test_base+caller_rvas[0],0);
}
bool world_lookup() noexcept {
    const auto saved_image=image;
    const auto saved_profile=compatibility::engine_profile;
    compatibility::engine_profile=compatibility::EngineProfile::october_patch;
    std::array<uintptr_t,2> gameplay{0,0x12340000},global_facts{0,0x56780000};
    std::array<uintptr_t,2> selectors{
        reinterpret_cast<uintptr_t>(gameplay.data()),
        reinterpret_cast<uintptr_t>(global_facts.data())};
    image=reinterpret_cast<uintptr_t>(selectors.data())-compatibility::mapped_address(0x5c20fe0)->current;
    bool ok=current_world()==gameplay[1];
    selectors[0]=0;ok=ok && !current_world(); // Never fall back to global facts.
    image=0;ok=ok && !current_world();
    image=saved_image;compatibility::engine_profile=saved_profile;
    return ok;
}
bool callthrough() {
    original=&stub;image=test_base;admission.store(0);deadline.store(GetTickCount64()+10000);
    std::array<unsigned char,0x28> left{},right{},ld{},rd{};
    put(left,8,story_id);put(right,8,uint64_t{2});put(ld,0x10,uint64_t{7});put(rd,0x10,uint64_t{9});
    left[0x1c]=4;right[0x1c]=2;ld[0x21]=1;rd[0x21]=2;
    uint8_t output=6;int logic{};
    const auto now=GetTickCount64();
    const auto admitted=admit(now);
    const auto result=forward(&logic,left.data(),ld.data(),right.data(),rd.data(),&output,admitted,test_base+caller_rvas[0],now);
    Event event{};
    bool ok=admitted && result==3 && output==17 && calls==1 &&
        seen==std::array<const void*,6>{&logic,left.data(),ld.data(),right.data(),rd.data(),&output} &&
        size==1 && queue[head].before.output==6 && queue[head].after.output==17 &&
        queue[head].stable && queue[head].result==3;
    const auto sampled=counts.samples.load();
    forward(&logic,left.data(),ld.data(),right.data(),rd.data(),&output,admit(now),test_base+caller_rvas[0],now);
    ok=ok && counts.samples.load()==sampled && counts.throttled.load() && size==1;
    put(right,8,uint64_t{8});
    forward(&logic,left.data(),ld.data(),right.data(),rd.data(),&output,admit(now),test_base+caller_rvas[0],now);
    ok=ok && counts.samples.load()==sampled+1 && size==2 && queue[(head+1)%queue_size].before.right.id==8;
    put(left,8,uint64_t{8});put(right,8,story_id);
    forward(&logic,left.data(),ld.data(),right.data(),rd.data(),&output,admit(now),test_base+caller_rvas[1],now);
    ok=ok && counts.samples.load()==sampled+2 && size==3 &&
        queue[(head+2)%queue_size].before.left.id==8 && queue[(head+2)%queue_size].caller_rva==caller_rvas[1];
    // Off-scope pairs are forwarded but never sampled.
    put(left,8,uint64_t{4});put(right,8,uint64_t{2});
    const auto previous=counts.samples.load();
    forward(&logic,left.data(),ld.data(),right.data(),rd.data(),&output,admit(now),test_base+caller_rvas[0],now);
    ok=ok && counts.samples.load()==previous && calls==5;
    // Caller failure must never dereference invalid engine pointers.
    forward(&logic,reinterpret_cast<void*>(1),ld.data(),right.data(),rd.data(),&output,admit(now),test_base+caller_rvas[0]+1,now);
    ok=ok && counts.caller_rejected.load() && calls==6;
    put(left,8,story_id);put(right,8,uint64_t{16});
    output=23;
    const auto rejected=counts.rejected.load();
    const auto unreadable_result=forward(&logic,left.data(),reinterpret_cast<void*>(1),right.data(),rd.data(),
                                          &output,admit(now),test_base+caller_rvas[0],now);
    ok=ok && unreadable_result==3 && output==17 && counts.rejected.load()>rejected &&
        size==4 && !queue[(head+3)%queue_size].before_valid && !queue[(head+3)%queue_size].stable && calls==7;
    put(left,8,story_id);fail=true;
    const auto before=counts.unwinds.load();
    ok=ok && catch_unwind(&logic,left.data(),ld.data(),right.data(),rd.data(),&output);
    fail=false;stop();
    output=31;
    const auto stopped=hook(&logic,left.data(),ld.data(),right.data(),rd.data(),&output);
    return ok && counts.unwinds.load()==before+1 && !(admission.load()&~closed) && !admit(GetTickCount64()) &&
        stopped==3 && output==17 && calls==9 &&
        seen==std::array<const void*,6>{&logic,left.data(),ld.data(),right.data(),rd.data(),&output};
}
bool invalid_memory() {
    Entry entry{};Definition definition{};uint8_t output{};
    return !read_entry(reinterpret_cast<void*>(1),entry) &&
        !read_definition(reinterpret_cast<void*>(1),definition) &&
        !read_output(reinterpret_cast<uint8_t*>(1),output);
}
bool bounds() {
    const auto previous=used.exchange(sample_limit);
    const bool limited=!sample_now(GetTickCount64(),0) && counts.budget_exhausted.load();
    used.store(previous);
    Event event{};event.returned=true;event.before_valid=true;event.after_valid=true;event.stable=true;
    AcquireSRWLockExclusive(&queue_lock);head=0;size=queue_size;ReleaseSRWLockExclusive(&queue_lock);
    const auto before=counts.dropped.load();
    admission.store(1);finish(event,true,nullptr,nullptr,nullptr,nullptr,nullptr);
    const bool full=counts.dropped.load()==before+1 && size==queue_size && !(admission.load()&~closed);
    AcquireSRWLockExclusive(&queue_lock);head=0;size=0;ReleaseSRWLockExclusive(&queue_lock);
    return limited && full;
}
bool reporting() {
    Event e{};e.sequence=1;e.time_ms=2;e.caller_rva=caller_rvas[0];e.returned=true;
    e.before_valid=e.after_valid=e.stable=true;e.before.left.id=story_id;e.result=1;
    std::ostringstream out;out<<std::hex<<std::showpos;
    event_json(out,e);totals_json(out,3,true);
    const auto value=out.str();
    return value.find("\"left_id\":\"67108864\"")!=std::string::npos &&
        value.find("\"caller_rva\":\"36510619\"")!=std::string::npos &&
        value.find("\"result\":1,\"conflict\":true")!=std::string::npos &&
        (out.flags()&std::ios::basefield)==std::ios::hex && (out.flags()&std::ios::showpos);
}
namespace {
void* seen_vm{};
int story_stub(void* vm) {
    seen_vm=vm;
    if(admission.load()&~closed) return -999;
    if(fail) RaiseException(test_exception,0,0,nullptr);
    return -17;
}
bool story_unwind(void* vm) {
    __try {story_hook(vm);}
    __except(GetExceptionCode()==test_exception?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
bool writer_frame_capture(void* vm);
}
bool reason_capture() {
    std::array<unsigned char,0x80> vm{},source{},name{};
    std::array<unsigned char,0x200> stack{};
    std::array<unsigned char,0x50> frames{};
    std::array<unsigned char,0x20> closure{};
    std::array<unsigned char,0xb0> proto{};
    std::array<uintptr_t,2> vm_context{fixture_world,0};
    const auto address=[](auto& a,size_t offset=0) {return reinterpret_cast<uintptr_t>(a.data())+offset;};
    constexpr size_t native_base=8*24;
    put(vm,8,address(stack,native_base+24));put(vm,0x10,address(stack,native_base));
    put(vm,0x20,address(frames,0x28));put(vm,0x28,address(stack,stack.size()));
    put(vm,0x30,address(stack));put(vm,0x40,address(frames));
    if(fixture_world) put(vm,0x78,address(vm_context));
    put(frames,0,address(stack,24));put(frames,8,address(stack));
    put(frames,0x28,address(stack,native_base));
    put(stack,0,address(closure));put(stack,0x10,uint32_t{7});
    closure[0]=7;put(closure,0x18,address(proto));
    put(proto,0x58,address(source));put(proto,0x60,address(name));
    put(proto,0x88,uint32_t{96});put(proto,0xa4,uint32_t{26});put(proto,0xa8,uint32_t{1});
    const char* labels[]{"story_mode_reasons_manager.lua","evaluate_story_mode_reasons"};
    std::array<unsigned char,0x80>* strings[]{&source,&name};
    for(size_t i=0;i<2;++i) {
        auto& s=*strings[i];s[0]=5;put(s,0x14,static_cast<uint32_t>(std::strlen(labels[i])));
        std::memcpy(s.data()+0x18,labels[i],std::strlen(labels[i]));
    }
    for(size_t i=0;i<6;++i) {put(stack,(i+1)*24,double(i==0?2:0));put(stack,(i+1)*24+0x10,uint32_t{3});}
    put(stack,native_base,int32_t{1});put(stack,native_base+0x10,uint32_t{1});
    const auto saved=stack;
    StoryReasons result{};
    bool ok=copy_story_reasons(vm.data(),result) && result.counts[0]==2 && result.active_mask==1 && result.enabled && stack==saved;
    put(stack,3*24,double{1});
    ok=ok && copy_story_reasons(vm.data(),result) && result.active_mask==5; // Concurrent conversation is preserved.
    put(stack,3*24,double{0});
    put(stack,24,double{-1});ok=ok && !copy_story_reasons(vm.data(),result) && result.active_mask==0;
    put(stack,24,double{0.5});ok=ok && !copy_story_reasons(vm.data(),result);
    put(stack,24,double{4294967296.0});ok=ok && !copy_story_reasons(vm.data(),result);
    put(stack,24,std::numeric_limits<double>::quiet_NaN());ok=ok && !copy_story_reasons(vm.data(),result);
    stack=saved;put(stack,native_base,int32_t{0});ok=ok && !copy_story_reasons(vm.data(),result);
    stack=saved;source[0x18]='X';ok=ok && !copy_story_reasons(vm.data(),result);source[0x18]='s';
    put(proto,0x88,uint32_t{95});ok=ok && !copy_story_reasons(vm.data(),result);put(proto,0x88,uint32_t{96});
    put(frames,0,address(stack,native_base));ok=ok && !copy_story_reasons(vm.data(),result);put(frames,0,address(stack,24));
    ok=ok && !copy_story_reasons(reinterpret_cast<void*>(1),result);
    original_story=&story_stub;admission.store(0);deadline.store(GetTickCount64()+10000);
    head=size=0;reason_calls=0;reason_samples=0;reason_rejected=0;reason_dropped=0;
    ok=ok && story_hook(vm.data())==-17 && seen_vm==vm.data() && size==1 && queue[0].reason_valid;
    std::ostringstream out;event_json(out,queue[0]);
    ok=ok && out.str().find("\"counts\":[2,0,0,0,0,0]")!=std::string::npos && stack==saved;
    fail=true;ok=ok && story_unwind(vm.data());fail=false;
    reason_calls=128;const auto samples=reason_samples.load();
    story_hook(vm.data());ok=ok && reason_samples.load()==samples;
    stop();ok=ok && story_hook(reinterpret_cast<void*>(1))==-17 && seen_vm==reinterpret_cast<void*>(1) && !(admission.load()&~closed);
    ok=writer_frame_capture(vm.data()) && ok;
    return ok;
}
namespace {
void* seen_writer_status{};
uint8_t seen_writer_mask{};
uint8_t seen_writer_enabled{};
bool writer_fail{},writer_skip_status{},writer_structural_change{},writer_world_change{};
unsigned writer_stub_calls{};
void writer_stub(void* status,uint8_t mask,uint8_t enabled) {
    seen_writer_status=status;seen_writer_mask=mask;seen_writer_enabled=enabled;++writer_stub_calls;
    if(writer_fail) RaiseException(test_exception,0,0,nullptr);
    if(writer_skip_status) return;
    auto* bytes=static_cast<uint8_t*>(status);
    if(enabled) bytes[1]|=mask;
    else bytes[1]&=static_cast<uint8_t>(~mask);
    if(writer_structural_change && structural_lifecycle::testing::begin_flush())
        structural_lifecycle::testing::end_flush();
    if(writer_world_change) fixture_world=fixture_other_world;
}
bool writer_unwind(void* status,uint8_t mask,uint8_t enabled,uintptr_t caller,uint64_t now) {
    __try {forward_writer(status,mask,enabled,admit(now),caller,now);}
    __except(GetExceptionCode()==test_exception?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
bool fixture_unwind(void* status,void* vm) {
    __try {crml_story_writer_fixture(status,1,0x80,vm);}
    __except(GetExceptionCode()==test_exception?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return true;}
    return false;
}
bool writer_frame_capture(void* vm) {
    original_writer=&writer_stub;
    image=reinterpret_cast<uintptr_t>(crml_story_writer_fixture_return)-story_writer_callers[0];
    admission=0;const auto now=GetTickCount64();capture_started=now;deadline=now+10000;
    for(auto& group:writer_used) group=0;
    head=size=0;writer_fail=false;writer_skip_status=false;
    std::array<uint8_t,2> status{4,4};
    uint8_t* target=status.data();ActionContext context{};
    if(fixture_world) {
        if(!copy_action_context(fixture_world,0,context)) return false;
        target=reinterpret_cast<uint8_t*>(context.status);
        structural_lifecycle::testing::arm();
    }
    const std::array<uint8_t,2> saved{target[0],target[1]};
    target[0]=4;target[1]=4;
    crml_story_writer_fixture(target,1,0x80,vm);
    bool ok=size==1 && queue[0].writer_manager_valid && queue[0].writer_manager_enabled_matches &&
        queue[0].reasons.counts==std::array<uint32_t,6>{2,0,0,0,0,0} &&
        queue[0].returned && target[1]==5 && seen_writer_enabled==0x80 &&
        queue[0].writer_context_unchanged==bool(fixture_world);
    std::ostringstream out;event_json(out,queue[0]);
    ok=ok && out.str().find("\"manager_counts\":[2,0,0,0,0,0]")!=std::string::npos;
    std::cout<<out.str(); // Actual producer record for the offline analyzer check.
    crml_story_writer_fixture(target,1,0,vm);
    ok=ok && size==2 && queue[1].writer_manager_valid && !queue[1].writer_manager_enabled_matches;
    // Non-manager masks and unknown callers must never dereference entry RBX.
    crml_story_writer_fixture(target,4,1,reinterpret_cast<void*>(1));
    ok=ok && size==3 && !queue[2].writer_manager_valid && target[1]==4;
    ++image;
    crml_story_writer_fixture(target,1,1,reinterpret_cast<void*>(1));
    ok=ok && size==4 && !queue[3].writer_manager_valid;
    --image;writer_fail=true;
    ok=fixture_unwind(target,vm) && ok;writer_fail=false;
    ok=ok && size==5 && !queue[4].returned && !(admission.load()&~closed);
    writer_structural_change=true;
    crml_story_writer_fixture(target,1,1,vm);
    writer_structural_change=false;
    ok=ok && size==6 && queue[5].writer_manager_valid && queue[5].returned && !queue[5].writer_context_unchanged;
    if(fixture_other_world) {
        const auto original_world=fixture_world;writer_world_change=true;
        crml_story_writer_fixture(target,1,1,vm);
        writer_world_change=false;fixture_world=original_world;
        ok=ok && size==7 && queue[6].writer_player_matches && queue[6].writer_manager_valid &&
            !queue[6].writer_context_unchanged;
    }
    const auto final_size=size;
    stop();const auto prior_calls=writer_stub_calls;
    crml_story_writer_fixture(target,1,0,reinterpret_cast<void*>(1));
    target[0]=saved[0];target[1]=saved[1];
    ok=ok && writer_stub_calls==prior_calls+1 && size==final_size;
    head=size=0;
    return ok;
}
}
bool writer_capture() {
    original_writer=&writer_stub;image=test_base;
    admission.store(0);deadline.store(GetTickCount64()+10000);
    for(auto& group:writer_used) group=0;
    writer_sequence=0;
    writer_counts.calls=0;writer_counts.samples=0;writer_counts.dropped=0;
    writer_counts.unreadable=0;writer_counts.unwinds=0;
    writer_counts.caller_unmapped=0;writer_counts.budget_exhausted=0;
    writer_stub_calls=0;writer_fail=false;writer_skip_status=false;
    AcquireSRWLockExclusive(&queue_lock);head=size=0;ReleaseSRWLockExclusive(&queue_lock);
    const auto now=GetTickCount64(),prior_occurrence=occurrence.load();
    std::array<uint8_t,2> status{0x35,0x10};
    const auto known=test_base+story_writer_callers[0];
    bool ok=writer_caller_matches(known,test_base) && !writer_caller_matches(test_base+story_writer_tail_jump+5,test_base) &&
        writer_caller_rva(test_base-1,test_base)==UINT32_MAX &&
        writer_caller_rva(test_base+0x12345,test_base)==0x12345;
    forward_writer(status.data(),4,true,admit(now),known,now);
    ok=ok && writer_stub_calls==1 && seen_writer_status==status.data() && seen_writer_mask==4 && seen_writer_enabled &&
        status==std::array<uint8_t,2>{0x35,0x14} && size==1 && queue[head].writer_event &&
        queue[head].writer_before==std::array<uint8_t,2>{0x35,0x10} &&
        queue[head].writer_after==status && queue[head].returned &&
        queue[head].sequence==1 && queue[head].occurrence==prior_occurrence+1 &&
        queue[head].caller_rva==story_writer_callers[0] && !queue[head].writer_player_context_valid;
    forward_writer(status.data(),2,0x80,admit(now),test_base+0x12345,now);
    ok=ok && status==std::array<uint8_t,2>{0x35,0x16} && seen_writer_enabled==0x80 &&
        size==2 && writer_counts.caller_unmapped.load()==1 &&
        queue[(head+1)%queue_size].caller_rva==0x12345 && queue[(head+1)%queue_size].writer_enabled &&
        queue[(head+1)%queue_size].occurrence==prior_occurrence+2;
    forward_writer(status.data(),6,0,admit(now),known,now);
    ok=ok && status==std::array<uint8_t,2>{0x35,0x10} && seen_writer_enabled==0 &&
        size==3 && !queue[(head+2)%queue_size].writer_enabled;
    writer_skip_status=true;
    forward_writer(reinterpret_cast<void*>(1),2,true,admit(now),known,now);
    writer_skip_status=false;
    ok=ok && size==4 && writer_counts.unreadable.load()==1 &&
        !queue[(head+3)%queue_size].writer_before_valid && !queue[(head+3)%queue_size].writer_after_valid;
    const auto before_unwinds=writer_counts.unwinds.load();writer_fail=true;
    ok=ok && writer_unwind(status.data(),8,true,known,now);
    writer_fail=false;
    ok=ok && writer_counts.unwinds.load()==before_unwinds+1 && size==5 &&
        !queue[(head+4)%queue_size].returned && !(admission.load()&~closed);
    std::ostringstream out;out<<std::hex<<std::showpos;
    event_json(out,queue[head]);event_json(out,queue[(head+4)%queue_size]);totals_json(out,now,true);
    const auto text=out.str();
    ok=ok && text.find("\"before_byte0\":53,\"after_byte0\":53,\"before_flags\":16,\"after_flags\":20")!=std::string::npos &&
        text.find("\"returned\":false,\"bytes_valid\":false")!=std::string::npos &&
        text.find("Capability story_writer: {\"schema\":1,\"type\":\"totals\"")!=std::string::npos &&
        (out.flags()&std::ios::basefield)==std::ios::hex && (out.flags()&std::ios::showpos);
    writer_used[writer_group(4)]=writer_group_limit;
    const auto samples=writer_counts.samples.load(),exhausted=writer_counts.budget_exhausted.load();
    forward_writer(status.data(),4,true,admit(now),known,now);
    ok=ok && writer_stub_calls==6 && writer_counts.samples.load()==samples &&
        writer_counts.budget_exhausted.load()==exhausted+1 && size==5;
    forward_writer(status.data(),1,true,admit(now),known,now);
    ok=ok && writer_stub_calls==7 && writer_counts.samples.load()==samples+1 &&
        size==6 && queue[(head+5)%queue_size].writer_mask==1;
    AcquireSRWLockExclusive(&queue_lock);head=0;size=queue_size;ReleaseSRWLockExclusive(&queue_lock);
    const auto dropped=writer_counts.dropped.load();
    forward_writer(status.data(),1,false,admit(now),known,now);
    ok=ok && writer_counts.dropped.load()==dropped+1 && size==queue_size;
    stop();writer_hook(status.data(),1,true);
    ok=ok && writer_stub_calls==9 && !(admission.load()&~closed);
    AcquireSRWLockExclusive(&queue_lock);head=size=0;ReleaseSRWLockExclusive(&queue_lock);
    return ok;
}
bool writer_budget() {
    constexpr uint64_t start=5000;
    capture_started=start;
    for(auto& group:writer_used) group=0;
    const auto throttled=writer_counts.throttled.load(),exhausted=writer_counts.budget_exhausted.load();
    for(unsigned i=0;i<8;++i) if(!sample_writer(4,start)) return false;
    for(unsigned i=0;i<100;++i) if(sample_writer(4,start+19999)) return false;
    if(writer_counts.throttled.load()!=throttled+100 || writer_counts.budget_exhausted.load()!=exhausted) return false;
    // Another category retains its own allowance. A late game action regains
    // a slot, while a stale timestamp cannot reopen a spent allowance.
    if(!sample_writer(1,start) || !sample_writer(4,start+20000) || sample_writer(4,start)) return false;
    for(unsigned i=9;i<writer_group_limit;++i) if(!sample_writer(4,start+480000)) return false;
    if(sample_writer(4,start+480000) || writer_counts.budget_exhausted.load()!=exhausted+1) return false;
    original_writer=&writer_stub;image=test_base;admission=0;deadline=start+capture_ms;
    const auto prior_calls=writer_stub_calls;
    const auto samples=writer_counts.samples.load();
    std::array<uint8_t,2> status{4,4};
    forward_writer(status.data(),4,false,admit(start+480000),test_base+story_writer_callers[0],start+480000);
    stop();
    return writer_stub_calls==prior_calls+1 && status[1]==0 && writer_counts.samples.load()==samples;
}
namespace {
unsigned comparator_calls{},policy_calls{};
unsigned policy_mode{};
unsigned pending_calls{};
unsigned warning_query_calls{};
uint64_t warning_seen_mask{};
bool no_pending(uint64_t) noexcept {++pending_calls;return false;}
uint8_t warning_query_stub(void*,uint64_t mask) {
    ++warning_query_calls;warning_seen_mask=mask;return mask!=0;
}
std::array<const void*,4> comparator_seen{};
uint8_t comparator_stub(void* logic,const void* requested,const void* interferer,uint8_t* output) {
    ++comparator_calls;comparator_seen={logic,requested,interferer,output};
    if(output) *output=0x5a;
    return 1;
}
uint64_t policy_stub(const Identity&,uint64_t) noexcept {
    ++policy_calls;
#ifdef CRML_PLAYER_STATUS_LIFECYCLE_TESTING
    if(policy_mode==1) player_status_lifecycle::testing::complete_copy();
#endif
    return 0x10033;
}
}
bool native_adapter() {
    const auto saved_original=original_comparator;
    const auto saved_image=image;
    const auto saved_tracking=tracking_enabled.load();
    const auto saved_policy=tracking_policy.load();
    const auto saved_pending=tracking_pending.load();
    original_comparator=&comparator_stub;image=test_base;
    tracking_policy.store(&policy_stub);tracking_pending.store(nullptr);tracking_enabled.store(true);
    comparator_calls=policy_calls=0;
    std::array<unsigned char,50*0x28> entries{};
    auto* requested=entries.data();auto* interferer=entries.data()+0x28;
    put(entries,8,uint64_t{2});put(entries,0x28+8,story_id);
    uint8_t output=0x37;
    bool ok=!native_exception(entries.data(),requested,interferer,test_base+comparator_return_rva+1,1) &&
            policy_calls==0;
    ok=ok && comparator_hook(entries.data(),requested,interferer,&output)==1 && output==0x5a &&
       comparator_calls==1 && comparator_seen==std::array<const void*,4>{entries.data(),requested,interferer,&output};
    put(entries,0x28+8,uint64_t{4});
    ok=ok && !native_exception(entries.data(),requested,interferer,test_base+comparator_return_rva,1) && policy_calls==0;
    put(entries,0x28+8,story_id);put(entries,8,uint64_t{8});
    ok=ok && !native_exception(entries.data(),requested,interferer,test_base+comparator_return_rva,1) && policy_calls==0;
    put(entries,8,uint64_t{2});
    tracking_pending.store(&no_pending);pending_calls=0;
    ok=ok && !native_exception(entries.data(),requested,interferer,test_base+comparator_return_rva,1) &&
       pending_calls==1 && policy_calls==0;
    tracking_pending.store(nullptr);
    ok=ok && !native_exception(entries.data(),requested,interferer,test_base+comparator_return_rva,1) && policy_calls==0;
    // The exact edge with an unverified player still forwards and never asks
    // the host for a lease. No component address survives this call.
    auto reasons=StoryReasons{};reasons.enabled=true;reasons.active_mask=1;reasons.counts[0]=1;
    const auto token=applied_story.begin_write();
    AppliedStoryState::Record record{123,456,0,0,0,0,reasons};
    applied_story.end_write(token,&record);
    ok=ok && applied_story.current(record)==false; // Local copy has no publication stamp.
    AppliedStoryState::Record stamped=record;stamped.writer_sequence=token+1;
    ok=ok && applied_story.current(stamped);
    admission.store(0);deadline.store(GetTickCount64()+1000);
    stop();
    ok=ok && tracking_enabled.load() && !active() && applied_story.current(stamped);
    original_writer=&writer_stub;
    const auto prior_samples=writer_counts.samples.load();
    std::array<uint8_t,2> status{4,1};
    writer_dispatch(status.data(),1,0,nullptr,test_base+0x123);
    ok=ok && status[1]==0 && writer_counts.samples.load()==prior_samples && !applied_story.current(stamped);
    tracking_enabled.store(false);tracking_policy.store(nullptr);
    original_comparator=saved_original;image=saved_image;
    tracking_enabled.store(saved_tracking);tracking_policy.store(saved_policy);
    tracking_pending.store(saved_pending);
    return ok;
}
bool native_positive() {
#ifndef CRML_PLAYER_STATUS_LIFECYCLE_TESTING
    return false;
#else
    auto address=[](auto& value) {return reinterpret_cast<uintptr_t>(value.data());};
    auto write=[](auto& value,size_t offset,auto scalar) {
        std::memcpy(value.data()+offset,&scalar,sizeof(scalar));
    };
    std::vector<unsigned char> world(0x58600),registry(0x60),chunk(0x1000),tags(1024),meta(16);
    std::array<unsigned char,4> slots{};
    std::array<uint32_t,2> hashes{0x9988f341,0x72d38548},offsets{0x80,0x900};
    std::array<uint64_t,1> generations{1},locations{0};
    write(world,0,address(registry));write(registry,0x48,address(slots));write(registry,0x50,uint32_t{1});
    write(world,0x58478,address(meta));write(meta,8,uint64_t{1});
    write(world,0x58480,address(tags));tags[0]=1;
    write(world,0x50,address(chunk));write(world,0x10448,uint32_t{1});
    write(world,0x584e8,address(generations));write(world,0x58510,uint64_t{1});
    write(world,0x58530,address(locations));
    write(chunk,0x10,uint64_t{1}<<32);chunk[0x900]=4;chunk[0x901]=1;
    write(world,0x18450,address(hashes));write(world,0x18458,address(offsets));
    write(world,0x18464,uint32_t{2});
    constexpr size_t request_index=1,story_index=26;
    write(chunk,0x80+request_index*0x28+8,uint64_t{2});
    write(chunk,0x80+story_index*0x28+8,story_id);
    std::array<uint32_t,3> env_hashes{1,0xe7097951,0xffffffff};
    std::array<uintptr_t,3> env_values{};
    std::array<unsigned char,0x130> dictionary{};
    std::array<unsigned char,64*40> buckets{};
    env_values[1]=address(dictionary);
    write(world,0x585b0,address(env_hashes));write(world,0x585b8,uint32_t{3});
    write(world,0x585c0,address(env_values));
    write(dictionary,0x48,uint64_t{64});write(dictionary,0x78,address(buckets));
    write(dictionary,0x50,UINT64_MAX);write(dictionary,0x58,UINT64_MAX);
    for(size_t i=0;i<64;++i) {write(buckets,i*40,UINT64_MAX);write(buckets,i*40+8,UINT64_MAX);}
    std::array<size_t,6> positions{};
    for(size_t i=0;i<story_fact_hashes.size();++i) {
        auto index=story_fact_hashes[i]&63;
        uint64_t step{};
        while(buckets[index*40]!=0xff) {++step;index=(index+step)&63;}
        positions[i]=index*40;
        write(buckets,index*40,uint64_t{0});write(buckets,index*40+8,story_fact_hashes[i]);
        write(buckets,index*40+0x10,uint16_t{1});write(buckets,index*40+0x18,double(i==0));
    }
    StoryReasons reasons{};
    bool ok=copy_dictionary_reasons(address(dictionary),reasons) && reasons.active_mask==1;
    const auto prior_image=image;const auto prior_world=fixture_world;
    const auto prior_tracking=tracking_enabled.load();
    const auto prior_policy=tracking_policy.load();
    const auto prior_pending=tracking_pending.load();
    const auto prior_query=original_active_query;
    image=test_base;fixture_world=address(world);
    structural_lifecycle::testing::arm();player_status_lifecycle::testing::arm();
    tracking_policy.store(&policy_stub);tracking_pending.store(nullptr);tracking_enabled.store(true);
    original_active_query=&warning_query_stub;warning_query_calls=0;
    const auto structural=structural_lifecycle::snapshot();
    const auto lifecycle=player_status_lifecycle::snapshot();
    AppliedStoryState::Record record{address(world),uint64_t{1}<<32,structural.teardowns,
        structural.unwinds,lifecycle.sequence,0,reasons};
    const auto logic=address(chunk)+0x80;
    const auto requested=reinterpret_cast<void*>(logic+request_index*0x28);
    const auto interferer=reinterpret_cast<void*>(logic+story_index*0x28);
    const auto skips=native_skips.load(),adjustments=warning_adjustments.load();policy_calls=policy_mode=0;
    for(size_t i=2;i<6;++i) {
        write(buckets,positions[i]+0x18,double{1});
        StoryReasons mixed{};
        ok=ok && copy_dictionary_reasons(address(dictionary),mixed);
        auto mixed_record=record;mixed_record.reasons=mixed;
        const auto mixed_token=applied_story.begin_write();applied_story.end_write(mixed_token,&mixed_record);
        ok=ok && !native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                                    test_base+comparator_return_rva,GetTickCount64());
        ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                                  GetTickCount64())==1 && warning_seen_mask==2 &&
           warning_adjustments.load()==adjustments;
        write(buckets,positions[i]+0x18,double{0});
    }
    const auto token=applied_story.begin_write();applied_story.end_write(token,&record);
    chunk[0x900]=8;chunk[0x901]=3;
    ok=ok && !native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                                test_base+comparator_return_rva,GetTickCount64());
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                              GetTickCount64())==1 && warning_seen_mask==2;
    chunk[0x900]=16;chunk[0x901]=5;
    ok=ok && !native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                                test_base+comparator_return_rva,GetTickCount64());
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                              GetTickCount64())==1 && warning_seen_mask==2;
    chunk[0x900]=4;chunk[0x901]=1;
    ok=ok && policy_calls==0 && native_skips.load()==skips;
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2|8,test_base+warning_return_rva+1,
                              GetTickCount64())==1 && warning_seen_mask==(2|8) && policy_calls==0;
    tracking_pending.store(&no_pending);pending_calls=0;
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                              GetTickCount64())==1 && warning_seen_mask==2 && pending_calls==1 && policy_calls==0;
    tracking_pending.store(nullptr);
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2|8,test_base+warning_return_rva,
                              GetTickCount64())==1 && warning_seen_mask==8 &&
       warning_adjustments.load()==adjustments+1 && policy_calls==1;
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                              GetTickCount64())==0 && warning_seen_mask==0 &&
       warning_adjustments.load()==adjustments+2 && policy_calls==2;
    policy_calls=0;
    ok=ok && native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                              test_base+comparator_return_rva,GetTickCount64()) &&
       policy_calls==1 && native_skips.load()==skips+1;
    // A correct ID in an incorrect slot is not the evaluator's current entry.
    write(chunk,0x80+2*0x28+8,uint64_t{2});
    ok=ok && !native_exception(reinterpret_cast<void*>(logic),reinterpret_cast<void*>(logic+2*0x28),
                                interferer,test_base+comparator_return_rva,GetTickCount64()) &&
       policy_calls==1;
    // A completed Status callback inside host policy must retire the applied
    // record even though the following read interval itself is quiet.
    policy_mode=1;
    ok=ok && !native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                                test_base+comparator_return_rva,GetTickCount64()) &&
       policy_calls==2 && native_skips.load()==skips+1;
    policy_mode=0;
    ok=ok && !native_exception(reinterpret_cast<void*>(logic),requested,interferer,
                                test_base+comparator_return_rva,GetTickCount64());
    record.status_sequence=player_status_lifecycle::snapshot().sequence;
    const auto fresh_token=applied_story.begin_write();applied_story.end_write(fresh_token,&record);
    policy_mode=1;
    ok=ok && warning_dispatch(reinterpret_cast<void*>(logic),2,test_base+warning_return_rva,
                              GetTickCount64())==1 && warning_seen_mask==2 &&
       warning_adjustments.load()==adjustments+2;
    policy_mode=0;
    tracking_enabled.store(false);tracking_policy.store(nullptr);applied_story.invalidate();
    structural_lifecycle::stop();player_status_lifecycle::stop();
    fixture_world=prior_world;image=prior_image;
    tracking_enabled.store(prior_tracking);tracking_policy.store(prior_policy);
    tracking_pending.store(prior_pending);
    original_active_query=prior_query;
    return ok;
#endif
}
}
#endif
}
