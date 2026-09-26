#pragma once
#include "entity_inspector.h"
#include <Windows.h>
#include <atomic>
#include <array>
#include <iomanip>
#include <ostream>

namespace crml::physics {
// Component identity checks do not assume a payload size for attachment tags.
// Unknown/unreadable entity metadata must not make an object eligible.
inline unsigned entity_exclusion(uintptr_t world,uint64_t entity,uint64_t player) noexcept {
    if(entity==player) return 1;
    uintptr_t chunk{};uint32_t row{};
    if(!probe::entity_component(world,entity,0x6cfbb2a9,32,chunk,row)) return 6;
    probe::Sample sample{};sample.world=world;sample.entity=entity;sample.row=row;
    probe::EntitySnapshot snapshot{};
    if(!probe::inspect_entity(sample,snapshot)) return 6;
    bool attached=false;
    for(uint32_t i=0;i<snapshot.count;++i) {
        if(snapshot.components[i].hash==0x9b382c56) return 2; // CharacterController
        if(snapshot.components[i].hash==0x787f4f88) attached=true; // heron::character_attachment::component::ItemAttached
    }
    return attached?5:0;
}
// Copies only identities, scalar coordinates and component hashes. No engine
// addresses or unknown component payloads are retained in this queue.
struct TargetDiagnostic {
    uint64_t search{},tick{},scene{},player{},entity{},body{};
    uint32_t thread{},body_count{},exclusion{};
    float player_position[3]{},position[3]{},distance{};
    bool player_sample{},components_valid{};
    probe::EntitySnapshot snapshot{};
};
class TargetDiagnostics {
public:
    static constexpr size_t capacity=16;
    bool push(const TargetDiagnostic& value) noexcept {
        if(!TryAcquireSRWLockExclusive(&lock_)) {++dropped_;return false;}
        const bool room=size_<capacity;
        if(room) {items_[(head_+size_)%capacity]=value;++size_;} else ++dropped_;
        ReleaseSRWLockExclusive(&lock_);return room;
    }
    bool pop(TargetDiagnostic& value) noexcept {
        if(!TryAcquireSRWLockExclusive(&lock_)) return false;
        const bool present=size_!=0;
        if(present) {value=items_[head_];head_=(head_+1)%capacity;--size_;}
        ReleaseSRWLockExclusive(&lock_);return present;
    }
    uint64_t dropped() const noexcept {return dropped_.load();}
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::array<TargetDiagnostic,capacity> items_{};
    size_t head_{},size_{};
    std::atomic<uint64_t> dropped_{};
};
inline void write_target_diagnostic(std::ostream& out,const TargetDiagnostic& d) {
    out<<std::setprecision(9)<<"{\"type\":\"target\",\"search\":"<<d.search<<",\"tick_ms\":"<<d.tick<<",\"thread\":"<<d.thread
       <<",\"scene\":\""<<d.scene<<"\",\"player\":\""<<d.player<<"\",\"entity\":\""<<d.entity<<"\",\"body\":\""<<d.body
       <<"\",\"role\":\""<<(d.player_sample?"player":"candidate")<<"\",\"body_count\":"<<d.body_count<<",\"exclusion\":"<<d.exclusion
       <<",\"distance\":"<<d.distance<<",\"player_position\":["<<d.player_position[0]<<','<<d.player_position[1]<<','<<d.player_position[2]
       <<"],\"position\":["<<d.position[0]<<','<<d.position[1]<<','<<d.position[2]
       <<"],\"components_valid\":"<<(d.components_valid?"true":"false")<<",\"components\":[";
    if(d.components_valid) for(uint32_t i=0;i<d.snapshot.count && i<d.snapshot.components.size();++i) {
        if(i) out<<',';
        out<<d.snapshot.components[i].hash;
    }
    out<<"]}\n";
}
}
