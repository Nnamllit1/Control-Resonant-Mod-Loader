#include "entity_inspector.h"
#include <Windows.h>
#include <cstring>
#include <iomanip>

namespace crml::probe {
namespace {
template<class T> T read(uintptr_t at) noexcept {
    T value{}; std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof(value)); return value;
}
bool collect(const Sample& s, EntitySnapshot& out) noexcept {
    if(s.rejection!=Rejection::none || !s.world || !s.entity || s.entity==UINT64_MAX) return false;
    const auto world=s.world;
    const auto index=static_cast<uint32_t>(s.entity);
    const auto capacity=read<uint64_t>(world+0x58510);
    if(capacity>16*1024*1024 || index>=capacity) return false;
    const auto generations=read<uintptr_t>(world+0x584e8);
    const auto locations=read<uintptr_t>(world+0x58530);
    if(!generations || !locations || read<uint32_t>(generations+index*8ull)!=s.entity>>32) return false;
    const auto location=read<uint64_t>(locations+index*8ull);
    const auto archetype=static_cast<uint16_t>(location);
    const auto row=static_cast<uint32_t>(location>>32);
    const auto count=read<uint64_t>(read<uintptr_t>(world+0x58478)+8);
    if(!count || count>8192 || archetype>=count || row>=16384 || row!=s.row) return false;
    const auto chunk=read<uintptr_t>(world+0x50+archetype*8ull);
    if(!chunk || row>=read<uint32_t>(world+0x10448+archetype*4ull) ||
       read<uint64_t>(chunk+0x10+row*8ull)!=s.entity) return false;
    const auto metadata=world+(archetype+0xc22ull)*32;
    // Only the world layout validated by the movement probe is supported.
    // Other static accessor layouts may take a registry, not this world.
    for(uint8_t table=1;table<2;++table) {
        const auto size=read<uint32_t>(metadata+(table?0x24:0x1c));
        if(size>2048) return false;
        const auto hashes=read<uintptr_t>(metadata+(table?0x10:8));
        if(size && !hashes) return false;
        for(uint32_t i=0;i<size;++i) {
            if(out.count==out.components.size()) { out.truncated=true; break; }
            out.components[out.count++]={read<uint32_t>(hashes+i*4ull),table};
        }
    }
    if(read<uint64_t>(locations+index*8ull)!=location ||
       read<uint32_t>(generations+index*8ull)!=s.entity>>32 ||
       read<uint64_t>(chunk+0x10+row*8ull)!=s.entity) return false;
    out.entity=s.entity; out.row=row; out.archetype=archetype; out.player=s;
    out.player.world=0; // No world pointer leaves the sampling thread.
    out.valid=true;
    return true;
}
const char* name(uint32_t hash) noexcept {
    switch(hash) {
    case 0x6cfbb2a9:return "WorldTransformReadOnly";
    case 0x9b382c56:return "CharacterController";
    case 0x1fda8b03:return "MaterialResourceID";
    case 0x0eee2128:return "RenderObject";
    case 0x355cf83d:return "MeshMaterialSet";
    case 0x58372dc9:return "GlobalID";
    case 0x39e33fda:return "AnimationOutput";
    default:return nullptr;
    }
}
}
bool inspect_entity(const Sample& sample, EntitySnapshot& result) noexcept {
    result={};
    bool ok=false;
    __try { ok=collect(sample,result); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { ok=false; }
    if(!ok) result={};
    return ok;
}
void write_entity_snapshot(std::ostream& out,const EntitySnapshot& s,uint64_t tick,uint32_t thread) {
    out << "{\"valid\":" << (s.valid?"true":"false") << ",\"tick_ms\":" << tick
        << ",\"thread\":" << thread << ",\"entity\":\"" << s.entity << "\",\"archetype\":" << s.archetype
        << ",\"row\":" << s.row << ",\"truncated\":" << (s.truncated?"true":"false") << ",\"components\":[";
    for(uint32_t i=0;i<s.count;++i) {
        if(i) out << ',';
        const auto& c=s.components[i];
        out << "{\"hash\":\"" << std::hex << std::setw(8) << std::setfill('0') << c.hash << std::dec
            << "\",\"table\":" << unsigned(c.table);
        if(const auto label=name(c.hash)) out << ",\"name\":\"" << label << '"';
        out << '}';
    }
    const auto& p=s.player;
    out << "],\"position\":[" << p.position[0] << ',' << p.position[1] << ',' << p.position[2]
        << "],\"controller_position\":[" << p.controller_position[0] << ',' << p.controller_position[1] << ',' << p.controller_position[2]
        << "],\"controller_disabled\":" << unsigned(p.disabled) << ",\"keyframed\":[" << unsigned(p.keyframed[0])
        << ',' << unsigned(p.keyframed[1]) << "],\"teleported\":" << unsigned(p.teleported) << "}\n";
}
}
