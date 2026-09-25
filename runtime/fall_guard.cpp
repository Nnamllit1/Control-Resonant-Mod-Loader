#include "fall_guard.h"
#include "movement_view.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace crml::probe::fall {
namespace {
using Inactive = void(*)(void*,void*,void*,void*);
using Target = void*(*)(void*,uint64_t,void*);
using FallAction = void(*)(void*,void*);
using ActiveRecovery = void(*)(void*,void*,void*,void*,void*,void*,void*,void*);
Inactive original_inactive{};
Target original_target{};
Inactive original_monitor{};
FallAction original_fall_action{};
ActiveRecovery original_active_recovery{};
Inactive original_camera{};
Active active_callback{};
std::atomic<bool> ready{};
std::atomic<uint64_t> checks{},triggers{};
std::atomic<uint64_t> monitors{},fall_actions{};
std::atomic<uint64_t> recoveries{},camera_calls{},camera_clears{};
constexpr uint32_t data_hash=0x6507c6a9, data_stride=240;

uintptr_t data(Player player,uintptr_t& chunk,uint32_t& row) noexcept {
    return entity_component(player.world,player.entity,data_hash,data_stride,chunk,row);
}
template<class T> T read(uintptr_t at) noexcept { T value; std::memcpy(&value,reinterpret_cast<const void*>(at),sizeof(value)); return value; }
bool inactive_inner(const void* view,Player player) noexcept {
    if(!view) return false;
    uintptr_t chunk{}; uint32_t row{};
    const auto component=data(player,chunk,row);
    if(!component) return false;
    const auto at=reinterpret_cast<uintptr_t>(view);
    return read<uintptr_t>(at+0x48)==chunk && read<uint64_t>(at+0x50)==row &&
           read<uintptr_t>(at+0x30)+row*uint64_t{data_stride}==component;
}
bool exclude_inner(void* result,const void* query,uint64_t entity,Player player) noexcept {
    if(!result || !query || !player.world || !player.entity || entity!=player.entity ||
       read<uintptr_t>(reinterpret_cast<uintptr_t>(query))!=player.world) return false;
    const auto at=reinterpret_cast<uintptr_t>(result);
    if(read<uint8_t>(at+0x38)!=1) return false;
    uintptr_t chunk{}; uint32_t row{};
    const auto component=data(player,chunk,row);
    if(!component || read<uintptr_t>(at+0x28)!=chunk || read<uint64_t>(at+0x30)!=row ||
       read<uintptr_t>(at+0x10)+row*uint64_t{data_stride}!=component) return false;
    // Same empty-result representation as the original query. No entity state
    // is changed; the trigger loop simply continues to its next target.
    *reinterpret_cast<uint8_t*>(at+0x38)=0;
    return true;
}
uintptr_t monitor_view(const void* view,Player player,bool monitoring) noexcept {
    if(!view) return 0;
    uintptr_t chunk{}; uint32_t row{};
    // update_monitoring: PlayerFallMonitorData at view[7], stride20.
    // update_triggered: the same component at view[6]. Both have 9 components.
    const auto state=entity_component(player.world,player.entity,0xc9909506,20,chunk,row);
    if(!state) return 0;
    const auto at=reinterpret_cast<uintptr_t>(view);
    if(read<uintptr_t>(at+0x48)!=chunk || read<uint64_t>(at+0x50)!=row ||
       read<uintptr_t>(at+(monitoring?0x38:0x30))+row*20ull!=state) return 0;
    const auto transform=entity_component(player.world,player.entity,0x6cfbb2a9,32,chunk,row);
    if(!transform || read<uintptr_t>(at)+row*32ull!=transform) return 0;
    return state;
}
bool monitor_inner(const void* view,Player player) noexcept {
    if(!monitor_view(view,player,true)) return false;
    uintptr_t chunk{}; uint32_t row{};
    const auto output=entity_component(player.world,player.entity,0x6869fc9d,8,chunk,row);
    if(!output || read<uintptr_t>(reinterpret_cast<uintptr_t>(view)+0x40)+row*8ull!=output) return false;
    // get_player_fall_data returns these two floats (distance and ground query
    // distance). Neutralize only this player's script-facing output on the game
    // system thread. Normal monitoring rewrites both on its next unsuppressed call.
    // Leave animation ownership, safe positions and the global fade renderer alone.
    constexpr float neutral[2]{};
    std::memcpy(reinterpret_cast<void*>(output),neutral,sizeof(neutral));
    return true;
}
void monitor(void* view,void* time,void* physics,void* events) {
    if(ready.load(std::memory_order_acquire) && suppress_monitor(view,active_callback())) { ++monitors; return; }
    original_monitor(view,time,physics,events);
}
void fall_action(void* view,void* events) {
    if(ready.load(std::memory_order_acquire) && matches_fall_action(view,active_callback())) { ++fall_actions; return; }
    original_fall_action(view,events);
}
void active_recovery(void* view,void* physics,void* time,void* state,void* damage,void* transition,void* events,void* counter) {
    if(ready.load(std::memory_order_acquire) && matches_active_recovery(view,active_callback())) { ++recoveries; return; }
    original_active_recovery(view,physics,time,state,damage,transition,events,counter);
}
void camera(void* view,void* mixer,void* requests,void* fade) {
    CameraOverride replacement;
    const bool replaced=ready.load(std::memory_order_acquire) && replacement.prepare(view,active_callback());
    if(replaced) ++camera_calls;
    // Always run the original: skipping it would leave an already-active fade
    // latched. Its inactive branch releases the fall camera and requests clear.
    original_camera(replaced?replacement.view.data():view,mixer,requests,fade);
}
void inactive(void* view,void* anomalies,void* trigger_query,void* physics) {
    if(ready.load(std::memory_order_acquire) && matches_inactive(view,active_callback())) { ++checks; return; }
    original_inactive(view,anomalies,trigger_query,physics);
}
void* target(void* result,uint64_t entity,void* query) {
    auto returned=original_target(result,entity,query);
    if(ready.load(std::memory_order_acquire) && exclude_target(returned,query,entity,active_callback())) ++triggers;
    return returned;
}
}
bool available(Player player) noexcept {
    __try {
        uintptr_t chunk{}; uint32_t row{};
        const auto component=data(player,chunk,row);
        // Match the inactive update's eligibility and reject pending recovery,
        // fade, and transition flags before taking ownership.
        return component && !read<uint8_t>(component+0xe4) && !(read<uint8_t>(component+0xe5)&7);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool matches_inactive(const void* view,Player player) noexcept {
    __try { return inactive_inner(view,player); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool exclude_target(void* result,const void* query,uint64_t entity,Player player) noexcept {
    __try { return exclude_inner(result,query,entity,player); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool suppress_monitor(const void* view,Player player) noexcept {
    __try { return monitor_inner(view,player); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool matches_fall_action(const void* view,Player player) noexcept {
    __try { return monitor_view(view,player,false)!=0; }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool matches_active_recovery(const void* view,Player player) noexcept {
    __try {
        if(!view) return false;
        uintptr_t chunk{}; uint32_t row{};
        const auto component=data(player,chunk,row);
        const auto at=reinterpret_cast<uintptr_t>(view);
        return component && read<uintptr_t>(at+0x48)==chunk && read<uint64_t>(at+0x50)==row &&
               read<uintptr_t>(at+0x20)+row*uint64_t{data_stride}==component;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool CameraOverride::prepare(const void* original,Player player) noexcept {
    __try {
        if(!original) return false;
        uintptr_t chunk{}; uint32_t row{};
        const auto component=data(player,chunk,row);
        if(!component) return false;
        std::memcpy(view.data(),original,sizeof(view));
        if(view[3]!=chunk || view[4]!=row || view[0]+row*uint64_t{data_stride}!=component) return false;
        const auto camera_data=entity_component(player.world,player.entity,0xa342c1c2,16,chunk,row);
        const auto config=entity_component(player.world,player.entity,0x56b782b6,32,chunk,row);
        if(!camera_data || !config || view[2]+row*16ull!=camera_data || view[1]+row*32ull!=config) return false;
        std::memcpy(recovery.data(),reinterpret_cast<const void*>(component),recovery.size());
        recovery[0xe5]&=0xfe; // Only the recovery-active bit used by this updater.
        view[0]=reinterpret_cast<uintptr_t>(recovery.data())-row*uint64_t{data_stride};
        if(read<uint8_t>(camera_data)) ++camera_clears;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool start(uintptr_t image,Active callback) noexcept {
    if(!image || !callback) return false;
    // Only called after the full executable SHA256 check in Recorder::start.
    auto check=reinterpret_cast<void*>(image+0x25a7230);
    auto query=reinterpret_cast<void*>(image+0x25ad320);
    auto monitor_check=reinterpret_cast<void*>(image+0x229ba00);
    auto action_check=reinterpret_cast<void*>(image+0x229b4b0);
    auto recovery_check=reinterpret_cast<void*>(image+0x25a7620);
    auto camera_check=reinterpret_cast<void*>(image+0x25a85f0);
    constexpr unsigned char check_signature[]{0x48,0x8b,0xc4,0x56,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x18,0x01,0x00,0x00};
    constexpr unsigned char query_signature[]{0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0xd9};
    constexpr unsigned char monitor_signature[]{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x53,0x56,0x57,0x41,0x54,0x41,0x55};
    constexpr unsigned char action_signature[]{0x4c,0x8b,0xdc,0x49,0x89,0x53,0x10,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    constexpr unsigned char recovery_signature[]{0x4c,0x8b,0xdc,0x4d,0x89,0x4b,0x20,0x49,0x89,0x53,0x10};
    constexpr unsigned char camera_signature[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20};
    if(std::memcmp(check,check_signature,sizeof(check_signature)) || std::memcmp(query,query_signature,sizeof(query_signature)) ||
       std::memcmp(monitor_check,monitor_signature,sizeof(monitor_signature)) || std::memcmp(action_check,action_signature,sizeof(action_signature)) ||
       std::memcmp(recovery_check,recovery_signature,sizeof(recovery_signature)) || std::memcmp(camera_check,camera_signature,sizeof(camera_signature))) return false;
    active_callback=callback;
    if(MH_CreateHook(check,reinterpret_cast<void*>(&inactive),reinterpret_cast<void**>(&original_inactive))!=MH_OK ||
       MH_CreateHook(query,reinterpret_cast<void*>(&target),reinterpret_cast<void**>(&original_target))!=MH_OK ||
       MH_CreateHook(monitor_check,reinterpret_cast<void*>(&monitor),reinterpret_cast<void**>(&original_monitor))!=MH_OK ||
       MH_CreateHook(action_check,reinterpret_cast<void*>(&fall_action),reinterpret_cast<void**>(&original_fall_action))!=MH_OK ||
       MH_CreateHook(recovery_check,reinterpret_cast<void*>(&active_recovery),reinterpret_cast<void**>(&original_active_recovery))!=MH_OK ||
       MH_CreateHook(camera_check,reinterpret_cast<void*>(&camera),reinterpret_cast<void**>(&original_camera))!=MH_OK) return false;
    if(MH_EnableHook(check)!=MH_OK || MH_EnableHook(query)!=MH_OK || MH_EnableHook(monitor_check)!=MH_OK || MH_EnableHook(action_check)!=MH_OK ||
       MH_EnableHook(recovery_check)!=MH_OK || MH_EnableHook(camera_check)!=MH_OK) return false;
    ready.store(true,std::memory_order_release);
    return true;
}
uint64_t skipped_checks() noexcept { return checks.load(std::memory_order_relaxed); }
uint64_t skipped_triggers() noexcept { return triggers.load(std::memory_order_relaxed); }
uint64_t skipped_monitors() noexcept { return monitors.load(std::memory_order_relaxed); }
uint64_t skipped_fall_actions() noexcept { return fall_actions.load(std::memory_order_relaxed); }
uint64_t skipped_active_recoveries() noexcept { return recoveries.load(std::memory_order_relaxed); }
uint64_t camera_overrides() noexcept { return camera_calls.load(std::memory_order_relaxed); }
uint64_t camera_clear_requests() noexcept { return camera_clears.load(std::memory_order_relaxed); }
}
