#include "gameplay_router.h"
#include "controller_hook.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
struct Service:crml::Gameplay {
    uint32_t bits{};
    uint64_t lease{};
    float query[4]{};
    unsigned player_reads{},camera_reads{},physics_reads{};
    uint64_t read_owner{},read_token{};
    uint32_t capabilities() const noexcept override {return bits;}
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    int player_read(crml_player_state& out) noexcept override {
        ++player_reads;out={};
        if(!(bits&CRML_CAP_PLAYER_READ)) return -1;
        out.version=1;out.generation=7;out.position[0]=12.f;return 1;
    }
    int camera_read(crml_camera_state& out) noexcept override {
        ++camera_reads;out={};
        if(!(bits&CRML_CAP_CAMERA_READ)) return -1;
        out.version=1;out.generation=11;out.mode=2;out.flags=CRML_CAMERA_STATE_POSE;out.position[2]=-4.f;return 1;
    }
    int physics_read(uint64_t owner,uint64_t token,crml_physics_state& out) noexcept override {
        ++physics_reads;read_owner=owner;read_token=token;out={};
        if(!(bits&CRML_CAP_PHYSICS_DAMPING)) return -1;
        if(owner!=lease) return -2;
        out.version=1;out.flags=CRML_PHYSICS_STATE_DAMPING;out.linear_damping=2.f;return 1;
    }
    int visibility_set(uint64_t owner,bool hide) noexcept override {
        if(!(bits&CRML_CAP_PLAYER_VISIBILITY)) return -1;
        if(lease && lease!=owner) return -2;
        lease=hide?owner:0;return hide?1:0;
    }
    int physics_select(uint64_t owner) noexcept override {
        if(!(bits&CRML_CAP_PHYSICS_DAMPING)) return -1;
        if(lease && lease!=owner) return -2;
        lease=owner;return 0;
    }
    void release(uint64_t owner) noexcept override {if(lease==owner) lease=0;}
    int physics_select_near(uint64_t owner,float x,float y,float z,float r) noexcept override {
        query[0]=x;query[1]=y;query[2]=z;query[3]=r;return physics_select(owner);
    }
};
int main() {
    try {
        require(crml::controller::test_dispatch(),"Shared controller must forward once before observing");
        Service player,physics;
        player.bits=CRML_CAP_PLAYER_VISIBILITY;physics.bits=CRML_CAP_PHYSICS_DAMPING;
        crml::GameplayRouter services(player,physics);
        require(services.capabilities()==(player.bits|physics.bits),"Union of available services");
        require(services.visibility_set(1,true)==1 && services.physics_select(2)==0,"Different owners can use separate services together");
        require(services.visibility_set(2,true)==-2 && services.physics_select(1)==-2,"Ownership remains independent");
        services.release(1);
        require(!player.lease && physics.lease==2,"Release must not affect another owner");
        physics.bits=0;
        require(services.capabilities()==player.bits && services.physics_select(3)==-1,"Partial startup must leave other services usable");
        services.release(2);require(!physics.lease,"Unavailable service still receives cleanup");
        physics.bits=CRML_CAP_PHYSICS_DAMPING;
        require(services.visibility_set(3,true)==1 && services.physics_select(3)==0,"One mod may own multiple services");
        services.release(3);require(!player.lease && !physics.lease,"One release cleans all caller leases");
        require(services.physics_select_near(4,1,2,-3,4)==0 && physics.lease==4 && !player.lease
            && physics.query[0]==1 && physics.query[1]==2 && physics.query[2]==-3 && physics.query[3]==4,
            "Region and owner route unchanged to physics service");
        services.release(4);
        {
            Service read_player,read_physics,read_camera;
            read_player.bits=CRML_CAP_PLAYER_READ;
            read_physics.bits=CRML_CAP_PHYSICS_DAMPING;
            read_camera.bits=CRML_CAP_CAMERA_READ;
            crml::GameplayRouter snapshots(read_player,read_physics,&read_camera);
            crml_player_state player_state{};
            crml_camera_state camera_state{};
            crml_physics_state physics_state{};
            require(snapshots.capabilities()==(CRML_CAP_PLAYER_READ|CRML_CAP_PHYSICS_DAMPING|CRML_CAP_CAMERA_READ),
                    "Read service availability missing from union");
            require(snapshots.player_read(player_state)==1 && player_state.generation==7 && player_state.position[0]==12.f,
                    "Player snapshot not routed to player provider");
            require(snapshots.camera_read(camera_state)==1 && camera_state.generation==11 && camera_state.mode==2 && camera_state.position[2]==-4.f,
                    "Camera snapshot not routed to camera provider");
            constexpr uint64_t owner=0x12345678abcdef01ull,token=0xfedcba9876543210ull;
            read_physics.lease=owner;
            require(snapshots.physics_read(owner,token,physics_state)==1 && physics_state.linear_damping==2.f
                    && read_physics.read_owner==owner && read_physics.read_token==token,
                    "Physics read owner or token changed during routing");
            require(read_player.player_reads==1 && !read_player.camera_reads && !read_player.physics_reads
                    && read_camera.camera_reads==1 && !read_camera.player_reads && !read_camera.physics_reads
                    && read_physics.physics_reads==1 && !read_physics.player_reads && !read_physics.camera_reads,
                    "Snapshot requested from wrong provider");
            require(snapshots.physics_read(owner+1,token,physics_state)==-2 && !physics_state.version && read_physics.lease==owner,
                    "Read routing bypassed provider ownership");
            read_camera.bits=0;
            require(snapshots.capabilities()==(CRML_CAP_PLAYER_READ|CRML_CAP_PHYSICS_DAMPING)
                    && snapshots.camera_read(camera_state)==-1 && !camera_state.version
                    && snapshots.player_read(player_state)==1,
                    "Unavailable camera disabled another read service");
            read_camera.lease=owner;
            snapshots.release(owner);
            require(!read_camera.lease && !read_physics.lease,"Unavailable camera provider missed owner cleanup");
            crml::GameplayRouter without_camera(read_player,read_physics);
            std::memset(&camera_state,0xff,sizeof(camera_state));
            const crml_camera_state zero{};
            require(without_camera.camera_read(camera_state)==-1 && std::memcmp(&camera_state,&zero,sizeof(camera_state))==0,
                    "Absent camera provider failed to clear whole output");
        }
        std::cout<<"Shared service tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
