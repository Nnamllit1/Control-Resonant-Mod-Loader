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
Inactive original_inactive{};
Target original_target{};
Active active_callback{};
std::atomic<bool> ready{};
std::atomic<uint64_t> checks{},triggers{};
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
        // Do not enter noclip once a respawn or its fade/transition is underway.
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
bool start(uintptr_t image,Active callback) noexcept {
    if(!image || !callback) return false;
    // Only called after the full executable SHA256 check in Recorder::start.
    auto check=reinterpret_cast<void*>(image+0x25a7230);
    auto query=reinterpret_cast<void*>(image+0x25ad320);
    constexpr unsigned char check_signature[]{0x48,0x8b,0xc4,0x56,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x18,0x01,0x00,0x00};
    constexpr unsigned char query_signature[]{0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0xd9};
    if(std::memcmp(check,check_signature,sizeof(check_signature)) || std::memcmp(query,query_signature,sizeof(query_signature))) return false;
    active_callback=callback;
    if(MH_CreateHook(check,reinterpret_cast<void*>(&inactive),reinterpret_cast<void**>(&original_inactive))!=MH_OK ||
       MH_CreateHook(query,reinterpret_cast<void*>(&target),reinterpret_cast<void**>(&original_target))!=MH_OK) return false;
    if(MH_EnableHook(check)!=MH_OK || MH_EnableHook(query)!=MH_OK) return false;
    ready.store(true,std::memory_order_release);
    return true;
}
uint64_t skipped_checks() noexcept { return checks.load(std::memory_order_relaxed); }
uint64_t skipped_triggers() noexcept { return triggers.load(std::memory_order_relaxed); }
}
