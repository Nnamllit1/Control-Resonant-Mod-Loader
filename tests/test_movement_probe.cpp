#include "movement_view.h"
#include "noclip.h"
#include "fall_guard.h"
#include <cmath>
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using crml::probe::Observation;
using crml::probe::Sample;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> void put(std::vector<unsigned char>& b, size_t at, T value) { std::memcpy(b.data()+at, &value, sizeof(value)); }
uintptr_t address(std::vector<unsigned char>& b) { return reinterpret_cast<uintptr_t>(b.data()); }
struct Fixture {
    std::vector<unsigned char> world = std::vector<unsigned char>(0x58600);
    std::vector<unsigned char> registry = std::vector<unsigned char>(0x80);
    std::vector<unsigned char> tags = std::vector<unsigned char>(8);
    std::vector<unsigned char> bits = std::vector<unsigned char>(1024);
    std::vector<unsigned char> meta = std::vector<unsigned char>(16);
    std::vector<unsigned char> locations = std::vector<unsigned char>(32);
    std::vector<unsigned char> generations = std::vector<unsigned char>(32);
    std::vector<unsigned char> hashes = std::vector<unsigned char>(44);
    std::vector<unsigned char> offsets = std::vector<unsigned char>(44);
    std::vector<unsigned char> chunk = std::vector<unsigned char>(4096);
    std::vector<unsigned char> physics = std::vector<unsigned char>(64);
    std::vector<unsigned char> movement = std::vector<unsigned char>(64);
    std::vector<unsigned char> globals = std::vector<unsigned char>(4);
    std::vector<unsigned char> global_values = std::vector<unsigned char>(8);
    std::vector<unsigned char> cameras = std::vector<unsigned char>(24);
    std::vector<unsigned char> camera_chunk = std::vector<unsigned char>(256);
    std::vector<unsigned char> camera_hashes = std::vector<unsigned char>(4);
    std::vector<unsigned char> camera_offsets = std::vector<unsigned char>(4);
    std::array<uintptr_t,19> view{};
    uintptr_t world_pointer{};
    uint32_t row;
    explicit Fixture(uint32_t r): row(r) {
        world_pointer = address(world);
        put(world,0,address(registry)); put(registry,0x48,address(tags)); put(registry,0x50,uint32_t{4});
        put(tags,6,uint16_t{0}); bits[0]=2;
        put(world,0x58480,address(bits)); put(world,0x58478,address(meta)); put(meta,8,uint64_t{2});
        put(world,0x58510,uint64_t{4}); put(world,0x584e8,address(generations)); put(generations,16,uint32_t{7});
        put(world,0x58530,address(locations)); put(locations,16,(uint64_t{row}<<32)|1);
        put(world,0x58,address(chunk)); put(world,0x1044c,row+1);
        put(chunk,0x10+row*8,(uint64_t{7}<<32)|2);
        const auto m=(1+0xc22)*32;
        put(world,m+0x10,address(hashes)); put(world,m+0x24,uint32_t{11}); put(world,0x18478,address(offsets));
        const std::array<uint32_t,11> h={0x6cfbb2a9,0x9b382c56,0x5077c6e3,0x6da4a5ae,0x2a16db09,0xc55ae319,0x6507c6a9,0xc9909506,0x6869fc9d,0xa342c1c2,0x56b782b6};
        const std::array<uint32_t,11> off={0x100,0x200,0x300,0x340,0x360,0x3b0,0x400,0x800,0x900,0xa00,0xb00};
        for(int i=0;i<11;++i) { put(hashes,i*4,h[i]); put(offsets,i*4,off[i]); }
        put(chunk,0x100+row*32+16,1.25f); put(chunk,0x100+row*32+20,2.5f); put(chunk,0x100+row*32+24,-3.0f);
        put(physics,16,1.25f); put(physics,20,2.5f); put(physics,24,-3.0f);
        put(chunk,0x200+row*32,address(physics)); put(chunk,0x208+row*32,address(movement)); put(movement,0x10,address(physics));
        view[0]=address(chunk)+0x200; view[7]=address(chunk)+0x300; view[11]=address(chunk)+0x100;
        view[8]=address(chunk)+0x360; view[14]=address(chunk)+0x3b0;
        put(chunk,0x360+row*8,uint64_t{0x1122334455667701});
        // Observed GlobalID differs from the runtime handle; it may also be absent.
        put(chunk,0x380,uint64_t{0x3b8ca7fcb370409c});
        view[13]=address(chunk)+0x380; view[17]=address(chunk); view[18]=row;
        put(world,0x585b0,address(globals)); put(world,0x585b8,uint32_t{1}); put(world,0x585c0,address(global_values));
        put(globals,0,uint32_t{0xfe90f7f8}); put(global_values,0,address(cameras));
        const uint64_t camera_id=(uint64_t{11}<<32)|1;
        put(cameras,4,camera_id); put(generations,8,uint32_t{11}); put(locations,8,uint64_t{1}<<32);
        put(world,0x50,address(camera_chunk)); put(world,0x10448,uint32_t{2}); put(camera_chunk,0x18,camera_id);
        put(world,0xc22*32+0x10,address(camera_hashes)); put(world,0xc22*32+0x24,uint32_t{1}); put(world,0x18458,address(camera_offsets));
        put(camera_hashes,0,uint32_t{0x46967561}); put(camera_offsets,0,uint32_t{0x40});
        // Camera row 1, yaw +90 degrees: right=-Z, forward=+X.
        put(camera_chunk,0x80+8,-1.f); put(camera_chunk,0x80+16,1.f); put(camera_chunk,0x80+24,1.f);
    }
    Observation inspect(Sample& sample) { return crml::probe::inspect(view.data(), &world_pointer, 3, sample); }
};

using Six = void(*)(void*,void*,void*,void*,void*,void*);
Six trampoline{};
volatile uintptr_t forwarded{};
unsigned intercepted{};
__declspec(noinline) void target(void* a,void* b,void* c,void* d,void* e,void* f) {
    forwarded = reinterpret_cast<uintptr_t>(a) + 2*reinterpret_cast<uintptr_t>(b) + 3*reinterpret_cast<uintptr_t>(c)
              + 4*reinterpret_cast<uintptr_t>(d) + 5*reinterpret_cast<uintptr_t>(e) + 6*reinterpret_cast<uintptr_t>(f);
}
void detour(void* a,void* b,void* c,void* d,void* e,void* f) { ++intercepted; trampoline(a,b,c,d,e,f); }
int main() {
    try {
        for (uint32_t row : {0u, 3u}) {
            Fixture f(row); Sample s{};
            const auto before=f.chunk;
            require(f.inspect(s)==Observation::player && s.row==row && s.position[1]==2.5f, "Valid player view rejected");
            namespace fall=crml::probe::fall;
            const fall::Player player{s.world,s.entity};
            const auto fall_base=address(f.chunk)+0x400;
            const auto fall_at=0x400+row*240;
            std::array<uintptr_t,11> inactive{};
            inactive[6]=fall_base; inactive[9]=address(f.chunk); inactive[10]=row;
            require(fall::available(player) && fall::matches_inactive(inactive.data(),player),"Valid fall guard rejected");
            require(!fall::matches_inactive(inactive.data(),{}),"Inactive lease bypassed fall recovery");
            ++inactive[10]; require(!fall::matches_inactive(inactive.data(),player),"Wrong fall row bypassed"); --inactive[10];
            ++inactive[6]; require(!fall::matches_inactive(inactive.data(),player),"Wrong fall component bypassed"); --inactive[6];
            f.chunk[fall_at+0xe4]=1; require(!fall::available(player),"Respawn in progress allowed activation"); f.chunk[fall_at+0xe4]=0;
            for(unsigned char flag:std::array<unsigned char,3>{1,2,4}) {
                f.chunk[fall_at+0xe5]=flag;
                require(!fall::available(player),"Pending fall transition allowed activation");
            }
            f.chunk[fall_at+0xe5]=0;
            std::array<uintptr_t,8> result{0,0,fall_base,0,0,address(f.chunk),row,1};
            const auto original_result=result;
            const auto original_chunk=f.chunk;
            uintptr_t query_world=player.world;
            require(!fall::exclude_target(result.data(),&query_world,player.entity+1,player),"Another entity excluded from boundary triggers");
            require(result==original_result,"Unrelated trigger result changed");
            ++query_world; require(!fall::exclude_target(result.data(),&query_world,player.entity,player),"Another world's boundary trigger excluded"); --query_world;
            require(fall::exclude_target(result.data(),&query_world,player.entity,player) && result[7]==0,"Player boundary target was not excluded");
            require(f.chunk==original_chunk,"Fall guard mutated saved recovery state");
            result[7]=1; require(result==original_result,"Fall guard modified fields beyond query validity");
            put(f.generations,16,uint32_t{8});
            require(!fall::available(player) && !fall::matches_inactive(inactive.data(),player) &&
                    !fall::exclude_target(result.data(),&query_world,player.entity,player),"Stale player bypassed fall recovery");
            put(f.generations,16,uint32_t{7});
            f.view[13]=0;
            require(f.inspect(s)==Observation::player,"Optional GlobalID used as runtime identity");
            require(f.chunk==before,"Observer modified game data");
            crml::probe::Override replacement;
            const auto view_before=f.view;
            require(replacement.prepare(f.view.data(),s,{10,20,30}),"Override rejected valid view");
            float coordinates[3];
            std::memcpy(coordinates,reinterpret_cast<void*>(replacement.view[11]+row*32ull+16),12);
            require(coordinates[0]==10 && coordinates[2]==30,"Engine row indexing did not reach private transform");
            require(*reinterpret_cast<uint8_t*>(replacement.view[7]+row*2ull)==1,"Private keyframed mode missing");
            const auto* push=reinterpret_cast<const uint8_t*>(replacement.view[8]+row*8ull);
            require(push[0]==0 && push[1]==0x77 && push[7]==0x11,"Contact pass not disabled privately or unrelated flags changed");
            require(f.chunk==before && f.view==view_before,"Override mutated original components or arguments");
            std::array<uintptr_t,11> monitor{},action{};
            monitor[0]=action[0]=address(f.chunk)+0x100;
            monitor[7]=action[6]=address(f.chunk)+0x800;
            monitor[8]=address(f.chunk)+0x900;
            monitor[9]=action[9]=address(f.chunk);
            monitor[10]=action[10]=row;
            const auto output_at=0x900+row*8;
            put(f.chunk,output_at,120.f); put(f.chunk,output_at+4,1000.f);
            const auto before_monitor=f.chunk;
            require(!fall::suppress_monitor(monitor.data(),{}) && !fall::matches_fall_action(action.data(),{}),"Expired lease suppressed fall detection");
            require(!fall::suppress_monitor(monitor.data(),{player.world,player.entity+1}),"Other player fall detection suppressed");
            ++monitor[10]; require(!fall::suppress_monitor(monitor.data(),player),"Wrong monitor row accepted"); --monitor[10];
            ++monitor[8]; require(!fall::suppress_monitor(monitor.data(),player),"Wrong script output accepted"); --monitor[8];
            ++monitor[0]; require(!fall::suppress_monitor(monitor.data(),player),"Wrong monitor transform accepted"); --monitor[0];
            ++monitor[7]; require(!fall::suppress_monitor(monitor.data(),player),"Wrong monitor state accepted"); --monitor[7];
            put(f.generations,16,uint32_t{8});
            require(!fall::suppress_monitor(monitor.data(),player) && !fall::matches_fall_action(action.data(),player),"Stale monitor player accepted");
            put(f.generations,16,uint32_t{7});
            require(f.chunk==before_monitor,"Rejected monitor changed game state");
            require(fall::suppress_monitor(monitor.data(),player) && fall::matches_fall_action(action.data(),player),"Valid monitor not suppressed");
            auto expected_monitor=before_monitor;
            put(expected_monitor,output_at,0.f); put(expected_monitor,output_at+4,0.f);
            require(f.chunk==expected_monitor,"Monitor changed data beyond two script-facing floats");
            ++action[6]; require(!fall::matches_fall_action(action.data(),player),"Wrong fall action component accepted"); --action[6];
            // After lease release, simulated native updates must survive unchanged.
            put(f.chunk,output_at,3.f); put(f.chunk,output_at+4,5.f);
            const auto resumed=f.chunk;
            require(!fall::suppress_monitor(monitor.data(),{}) && f.chunk==resumed,"Normal fall output suppressed after release");
            put(f.chunk,output_at,0.f); put(f.chunk,output_at+4,0.f);
            for(size_t i=0;i<f.view.size();++i) if(i!=7 && i!=8 && i!=11 && i!=14) require(replacement.view[i]==f.view[i],"Override changed unrelated argument");
            f.chunk[0x3b0+row]=1; s.teleported=1;
            require(replacement.prepare(f.view.data(),s,{10,20,30}) &&
                    *reinterpret_cast<const uint8_t*>(replacement.view[14]+row)==0,"Teleport branch bypassed private noclip movement");
            require(f.chunk[0x3b0+row]==1 && f.view==view_before,"Override erased the real engine teleport flag");
            f.chunk[0x3b0+row]=0; s.teleported=0;
            crml::probe::CameraBasis camera;
            require(crml::probe::inspect_camera(f.world_pointer,camera) && camera.valid && camera.forward[0]==1,"Valid camera row rejected");
            const auto forward=crml::probe::camera_relative({0,0,1,false},camera);
            const auto right=crml::probe::camera_relative({1,0,0,false},camera);
            require(forward.x==1 && forward.z==0 && right.z==-1,"Camera-relative WASD axes incorrect");
            put(f.generations,8,uint32_t{12});
            require(!crml::probe::inspect_camera(f.world_pointer,camera) && !camera.valid,"Stale camera accepted");
            put(f.generations,8,uint32_t{11}); put(f.camera_chunk,0x80+8,std::numeric_limits<float>::quiet_NaN());
            require(!crml::probe::inspect_camera(f.world_pointer,camera),"NaN camera accepted");
            f.bits[0]=0; require(f.inspect(s)==Observation::other_entity,"Non-player accepted"); f.bits[0]=2;
            put(f.generations,16,uint32_t{8}); require(f.inspect(s)==Observation::invalid && s.rejection==crml::probe::Rejection::generation,"Stale entity rejection reason missing"); put(f.generations,16,uint32_t{7});
            f.view[18]=16384; require(f.inspect(s)==Observation::invalid,"Out-of-range row accepted"); f.view[18]=row;
            ++f.view[11]; require(f.inspect(s)==Observation::invalid,"Mismatched transform accepted"); --f.view[11];
            put(f.chunk,0x100+row*32+16,std::numeric_limits<float>::quiet_NaN()); require(f.inspect(s)==Observation::invalid,"NaN accepted");
            put(f.chunk,0x100+row*32+16,1.25f);
            put(f.movement,0x10,uintptr_t{0}); require(f.inspect(s)==Observation::invalid,"Mismatched controller accepted");
        }
        for(uint32_t row:{0u,3u}) {
            Fixture f(row); Sample sample{}; require(f.inspect(sample)==Observation::player,"Camera fixture player invalid");
            namespace fall=crml::probe::fall;
            const fall::Player player{sample.world,sample.entity};
            std::array<uintptr_t,11> active{};
            active[4]=address(f.chunk)+0x400; active[9]=address(f.chunk); active[10]=row;
            require(fall::matches_active_recovery(active.data(),player),"Active recovery view rejected");
            require(!fall::matches_active_recovery(active.data(),{}),"Inactive noclip skipped recovery");
            ++active[4]; require(!fall::matches_active_recovery(active.data(),player),"Mismatched recovery component accepted"); --active[4];
            std::array<uintptr_t,5> camera_view{address(f.chunk)+0x400,address(f.chunk)+0xb00,address(f.chunk)+0xa00,address(f.chunk),row};
            const auto recovery_at=0x400+row*240;
            f.chunk[recovery_at+0xe5]=0xa7;
            f.chunk[0xa00+row*16]=1; // Already-latched fall camera/fade.
            const auto before=f.chunk;
            fall::CameraOverride replacement;
            require(!replacement.prepare(camera_view.data(),{}),"Expired lease replaced fall camera");
            require(!replacement.prepare(camera_view.data(),{player.world,player.entity+1}),"Foreign fall camera replaced");
            ++camera_view[4]; require(!replacement.prepare(camera_view.data(),player),"Wrong camera row accepted"); --camera_view[4];
            for(size_t slot:{0u,1u,2u,3u}) {
                ++camera_view[slot]; require(!replacement.prepare(camera_view.data(),player),"Wrong camera component accepted"); --camera_view[slot];
            }
            put(f.generations,16,uint32_t{8});
            require(!replacement.prepare(camera_view.data(),player) && !fall::matches_active_recovery(active.data(),player),"Stale recovery generation accepted");
            put(f.generations,16,uint32_t{7});
            const auto clears=fall::camera_clear_requests();
            require(replacement.prepare(camera_view.data(),player),"Valid fall camera replacement rejected");
            require(fall::camera_clear_requests()==clears+1,"Latched camera cleanup not diagnosed");
            const auto private_data=reinterpret_cast<const uint8_t*>(replacement.view[0]+row*240ull);
            for(size_t i=0;i<240;++i) require(private_data[i]==(i==0xe5?0xa6:before[recovery_at+i]),"Camera override changed more than active bit");
            for(size_t i=1;i<5;++i) require(replacement.view[i]==camera_view[i],"Camera cleanup lost native output/config pointers");
            require(f.chunk==before,"Private camera view mutated game recovery state");
        }
        Sample s{};
        s.entity=42; s.world=100;
        crml::probe::Flight flight;
        const auto arm=[&] { flight.reset(); flight.enabled=true; flight.owner=1; flight.entity=42; flight.world=100; flight.lease=1000; flight.last_step=1000; };
        std::array<float,3> next{};
        arm(); require(flight.step(s,100,1050,true,{1,1,1,false},next),"Valid flight rejected");
        require(std::abs(std::sqrt(next[0]*next[0]+next[1]*next[1]+next[2]*next[2])-.25f)<.001f,"Diagonal speed is not normalized");
        arm(); require(!flight.step(s,100,1501,true,{},next) && !flight.enabled,"Expired lease retained flight");
        arm(); require(!flight.step(s,100,1050,false,{},next) && !flight.enabled,"Focus loss retained flight");
        arm(); require(!flight.step(s,101,1050,true,{},next),"World replacement retained flight");
        arm(); s.entity=43; require(!flight.step(s,100,1050,true,{},next),"Player replacement retained flight"); s.entity=42;
        arm(); s.keyframed[0]=1; require(!flight.step(s,100,1050,true,{},next),"Engine keyframing retained flight"); s.keyframed[0]=0;
        arm(); require(!flight.step(s,100,1300,true,{},next),"Long frame gap retained flight");
        arm(); s.teleported=1; require(!flight.step(s,100,1050,true,{},next),"Engine teleport retained flight"); s.teleported=0;
        arm();
        // Regression: an engine ground clamp must not erase accumulated descent each tick.
        for(uint64_t now=1050;now<=1400;now+=50) {
            flight.lease=now;
            require(flight.step(s,100,now,true,{0,-1,0,false},next),"Descent cancelled by a small floor correction");
        }
        require(std::abs(next[1]+2.f)<.001f,"Descent kept resetting to the floor");
        flight.lease=1450; require(flight.step(s,100,1450,true,{},next) && next[1]==-2.f,"No-input flight drifted back toward ground");
        s.position[0]=100;
        require(!flight.step(s,100,1500,true,{},next) && !flight.enabled,"Large relocation dragged player to stale flight target"); s.position[0]=0;
        require(flight.stopped.reason==crml::probe::StopReason::displacement && flight.stopped.tick==1500 &&
                flight.stopped.observed[0]==100 && flight.stopped.requested[1]==-2.f,"Displacement cancellation lost evidence");
        const auto stopped=flight.stopped;
        flight.reset(crml::probe::StopReason::focus,1600,&s);
        require(flight.stopped.count==stopped.count && flight.stopped.reason==stopped.reason,"Inactive cleanup erased stop cause");
        arm(); s.teleported=1; require(!flight.step(s,100,1050,true,{},next) && flight.stopped.reason==crml::probe::StopReason::teleport,"Teleport stop reason missing"); s.teleported=0;
        arm(); require(!flight.step(s,100,1501,true,{},next) && flight.stopped.reason==crml::probe::StopReason::lease,"Lease stop reason missing");
        // Recorded reset: unchanged entity/world and altitude, 94-unit sideways
        // engine teleport. Retain the flight target instead of cancelling F6.
        arm();
        s.position[0]=-2068.3772f; s.position[1]=-6.91603994f; s.position[2]=1971.72351f;
        require(flight.step(s,100,1033,true,{},next),"Flight did not acquire initial position");
        require(!flight.teleport_blocks(1),"Worker would cancel a positioned flight on teleport");
        s.teleported=1; s.position[0]=-2002.07007f; s.position[2]=2038.81567f;
        require(flight.step(s,100,1066,true,{},next) && flight.enabled,"Recorded engine reset disabled active flight");
        require(next[0]==-2068.3772f && next[1]==-6.91603994f && next[2]==1971.72351f,"Engine reset replaced flight target");
        require(flight.restored.count==1 && flight.restored.observed[0]==s.position[0] && flight.restored.requested==next,"Teleport restoration evidence missing");
        std::copy(next.begin(),next.end(),s.position);
        require(flight.step(s,100,1099,true,{},next) && flight.restored.count==1,"Unmoved flagged sample counted as another reset");
        s.teleported=0;
        require(flight.step(s,100,1132,true,{1,0,0,false},next) && next[0]>-2068.3772f,"Flight did not continue after reset correction");
        s.teleported=1;
        require(!flight.step(s,101,1165,true,{},next) && flight.stopped.reason==crml::probe::StopReason::world,"Teleport retained flight across a world change");
        require(flight.teleport_blocks(1),"Inactive flight accepted a teleport as an activation point");
        for(int guard=0;guard<5;++guard) {
            s={}; s.entity=42; s.world=100; arm();
            require(flight.step(s,100,1033,true,{},next),"Guard fixture did not acquire position");
            s.teleported=1; s.position[0]=94;
            if(guard==0) s.entity=43;
            if(guard==1) s.disabled=1;
            if(guard==2) s.keyframed[0]=1;
            const auto now=guard==3?1600ull:1066ull;
            require(!flight.step(s,100,now,guard!=4,{},next) && !flight.enabled,"Teleport restoration bypassed a lifecycle guard");
        }
        s={}; s.entity=42; s.world=100;
        const auto missing_camera=crml::probe::camera_relative({1,-1,1,true},{});
        require(missing_camera.x==0 && missing_camera.y==-1 && missing_camera.z==0 && missing_camera.fast,"Missing camera used guessed world axes");
        for(const auto& axes:std::array<std::array<float,2>,4>{{{1,0},{0,-1},{-1,0},{0,1}}}) {
            crml::probe::CameraBasis camera{}; camera.valid=true; camera.right[0]=axes[0]; camera.right[2]=axes[1];
            const auto w=crml::probe::camera_relative({0,0,1,false},camera);
            const auto a=crml::probe::camera_relative({-1,0,0,false},camera);
            const auto back=crml::probe::camera_relative({0,0,-1,false},camera);
            const auto d=crml::probe::camera_relative({1,0,0,false},camera);
            require(w.x==-axes[1] && w.z==axes[0] && w.y==0,"Forward did not follow camera yaw");
            require(w.x==-back.x && w.z==-back.z && a.x==-d.x && a.z==-d.z,"Opposing movement keys were not symmetric");
            require(std::abs(w.x*d.x+w.z*d.z)<.001f,"Camera forward and strafe were not perpendicular");
        }
        require(crml::probe::inspect(nullptr,nullptr,3,s)==Observation::invalid,"Null view accepted");
        auto page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
        require(page!=nullptr,"Guard page allocation failed");
        const auto fault=crml::probe::inspect(page,page,3,s);
        crml::probe::fall::CameraOverride camera_fault;
        require(!camera_fault.prepare(page,{reinterpret_cast<uintptr_t>(page),1}) &&
                !crml::probe::fall::matches_active_recovery(page,{reinterpret_cast<uintptr_t>(page),1}),"Unreadable recovery view accepted");
        require(!crml::probe::fall::matches_inactive(page,{reinterpret_cast<uintptr_t>(page),1}) &&
                !crml::probe::fall::suppress_monitor(page,{reinterpret_cast<uintptr_t>(page),1}) &&
                !crml::probe::fall::matches_fall_action(page,{reinterpret_cast<uintptr_t>(page),1}) &&
                !crml::probe::fall::exclude_target(page,page,1,{reinterpret_cast<uintptr_t>(page),1}) &&
                !crml::probe::fall::available({reinterpret_cast<uintptr_t>(page),1}),"Unreadable fall state accepted");
        VirtualFree(page,0,MEM_RELEASE);
        require(fault==Observation::invalid && s.rejection==crml::probe::Rejection::memory,"Unreadable memory rejection reason missing");
        require(MH_Initialize()==MH_OK,"MinHook initialization failed");
        require(MH_CreateHook(reinterpret_cast<void*>(&target),reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&trampoline))==MH_OK,"Hook creation failed");
        require(MH_EnableHook(reinterpret_cast<void*>(&target))==MH_OK,"Hook activation failed");
        Six volatile invoke=&target;
        invoke(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6));
        require(intercepted==1 && forwarded==91,"Hook lost register or stack arguments");
        require(MH_DisableHook(reinterpret_cast<void*>(&target))==MH_OK,"Hook disable failed");
        require(MH_Uninitialize()==MH_OK,"Hook cleanup failed");
        std::cout << "Movement guards, private overrides, flight cleanup, and six-argument trampoline passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
