#include "../runtime/media_hook.cpp"
#include <iostream>
#include <stdexcept>

#define CHECK(value) do {if(!(value)) throw std::runtime_error("Media hook check failed: " #value);} while(false)
int main() {
    try {
        using namespace crml::media;
        const char* path="textures\\videos\\uiresources\\splash\\boot.tex";
        image=reinterpret_cast<uintptr_t>(&path)-0x5a0b018;
        elapsed=+[](void*)->uint32_t {return 2001;};
        auto& service=process_service();service.enable(true);
        auto now=GetTickCount64();
        const auto route=[&](void* object,uintptr_t caller,bool active) {now+=16;return route_at(object,caller,active,now);};
        int object{};crml_media_state state{};
        const auto before=service.observations();
        CHECK(route(&object,image+123,true));
        CHECK(!route(&object,image+123,false));
        CHECK(service.observations()==before); // Unrelated callers are untouched.
        CHECK(route(&object,image+0x3d4c8d,true));
        CHECK(service.read_at(state,now)==1);
        CHECK(state.flags==(CRML_MEDIA_ACTIVE|CRML_MEDIA_SKIPPABLE|CRML_MEDIA_MAPPED_NAME));
        CHECK(std::string_view(state.name)==path);
        CHECK(route_at(&object,image+0x3d4c0d,true,now));
        const auto sampled=service.observations();
        for(unsigned i=0;i<1000;++i) CHECK(route_at(&object,image+0x3d4c0d,true,now+1));
        CHECK(service.observations()==sampled && service.skips()==0);
        CHECK(service.request_at(1,state.generation,now)==0);
        CHECK(route(&object,image+0x3d4c8d,true)); // Input query is observational.
        CHECK(service.skips()==0);
        // Input may sample first on every interval; it must not starve the
        // immediately following eligible query at exactly the same time.
        CHECK(!route_at(&object,image+0x3d4c0d,true,now));
        CHECK(service.skips()==1 && service.read_at(state,now)<0);
        CHECK(route(&object,image+0x3d4c0d,true)); // Never force a later query.
        CHECK(!route(&object,image+0x3d4c0d,false)); // Natural completion unchanged.
        elapsed=+[](void*)->uint32_t {return 2000;};
        CHECK(route(&object,image+0x3d4c0d,true));
        CHECK(service.read_at(state,now)==1 && !(state.flags&CRML_MEDIA_SKIPPABLE));
        CHECK(service.request_at(1,state.generation,now)<0);
        path=nullptr;CHECK(route(&object,image+0x3d4c0d,true));
        CHECK(service.read_at(state,now)<0); // Unreadable metadata never skips.
        service.enable(false);
        CHECK(route(&object,image+0x3d4c0d,true));
        // The patched executable moves both call sites and the resource-name
        // global. A stale caller must never receive authority to skip playback.
        for(const auto sha:{crml::compatibility::updated_sha,crml::compatibility::hotfix_sha}) {
        crml::compatibility::select_profile(sha);
        path="textures\\videos\\uiresources\\splash\\boot.tex";
        image=reinterpret_cast<uintptr_t>(&path)-(sha==crml::compatibility::hotfix_sha?0x5ae0018:0x5ad8018);
        elapsed=+[](void*)->uint32_t {return 2001;};
        service.enable(true);
        const auto updated_before=service.observations();
        CHECK(route(&object,image+0x3d4c0d,true));
        CHECK(service.observations()==updated_before);
        CHECK(route(&object,image+0x3f0f7d,true));
        CHECK(service.read_at(state,now)==1 && std::string_view(state.name)==path);
        CHECK(service.request_at(1,state.generation,now)==0);
        CHECK(route(&object,image+0x3f0f7d,true));
        CHECK(!route_at(&object,image+0x3f0efd,true,now));
        CHECK(service.read_at(state,now)<0);
        service.enable(false);
        }
        crml::compatibility::select_profile(crml::compatibility::tested_sha);
        std::cout<<"Media caller isolation, native skip point and metadata checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
