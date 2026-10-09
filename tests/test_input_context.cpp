#include "diagnostics/input_context.h"
#include "compatibility.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace crml::input_context {bool test_addresses(uintptr_t&,uintptr_t&,uintptr_t&) noexcept;}
using namespace crml::input_context;
void require(bool yes,const char* why) {if(!yes) throw std::runtime_error(why);}
template<class B,class T> void put(B& b,size_t at,T value) {std::memcpy(b.data()+at,&value,sizeof(value));}
int main() {
    try {
        std::array<unsigned char,0x580> object{};
        std::array<unsigned char,0x5040> backend{};
        std::array<unsigned char,0x18> source{};
        std::array<uintptr_t,2> predicate{};
        const auto at=reinterpret_cast<uintptr_t>(object.data()),bt=reinterpret_cast<uintptr_t>(backend.data());
        // Identity checks compare the vtable only; no method pointer is called.
        const uintptr_t vt=0x12345678;
        predicate={vt,at};
        put(object,0,uintptr_t{1}); // Non-null connection; never dereferenced.
        put(object,0x148,bt);put(object,0x150,uint64_t{17});put(backend,0x5028,uint64_t{17});
        put(object,0x1d8,reinterpret_cast<uintptr_t>(predicate.data()));
        put(object,0x198,reinterpret_cast<uintptr_t>(source.data()));source[0x10]=1;
        put(object,0x570,uint32_t{0xc10});put(object,0x578,uint16_t{3});
        const auto saved=object;const auto saved_backend=backend;const auto saved_source=source;const auto saved_predicate=predicate;
        Snapshot out{};
        require(inspect(at,at,vt,out)==Read::ok && out.raw_flags==0xc10 && out.derived_flags==3 &&
                out.connected && out.backend && out.source_checked && out.source && out.generation_matches && out.predicate_enabled && out.thread==GetCurrentThreadId(),"copied context fields");
        require(object==saved && backend==saved_backend && source==saved_source && predicate==saved_predicate,"observer wrote engine memory");
        put(object,0x570,uint32_t{0xc00});put(object,0x578,uint16_t{0});
        require(inspect(at,at,vt,out)==Read::ok && !out.predicate_enabled && out.raw_flags==0xc00,"raw controls query must not fabricate derived eligibility");
        put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::ok && !out.source_checked,"disabled predicate must not dereference fallback source");
        object=saved;
        put(object,0x150,uint64_t{16});
        put(object,0x1d8,uintptr_t{1});put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::ok && !out.generation_matches,"stale generation is visible");
        require(!out.source_checked && !out.predicate_enabled,"stale generation must not follow predicate or source pointers");
        object=saved;
        put(object,0x150,UINT64_MAX);
        require(inspect(at,at,vt,out)==Read::ok && out.generation_matches,"native all-ones generation wildcard");
        source[0x10]=0;require(inspect(at,at,vt,out)==Read::ok && !out.source,"inactive input source");
        put(object,0x188,uintptr_t{1});put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::ok && out.source,"alternative source avoids dereferencing unused fallback");
        object=saved;
        predicate[0]=vt+8;require(inspect(at,at,vt,out)==Read::predicate && !out.raw_flags && !out.thread,"foreign predicate must clear output");
        predicate=saved_predicate;predicate[1]=at+8;
        require(inspect(at,at,vt,out)==Read::predicate,"foreign predicate owner");predicate=saved_predicate;
        put(object,0x1d8,uintptr_t{0});require(inspect(at,at,vt,out)==Read::predicate,"active backend without predicate");
        put(object,0x148,uintptr_t{0});
        put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::ok && !out.backend && !out.generation_matches,"disconnected view can have an empty predicate");
        object=saved;
        put(object,0,uintptr_t{0});put(object,0x1d8,uintptr_t{1});put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::ok && out.raw_flags==0xc10 && !out.connected && !out.backend && !out.predicate_enabled,
                "disconnected controls select the fallback despite stale populated view data");
        object=saved;
        require(inspect(at,at+8,vt,out)==Read::identity && !out.raw_flags,"unexpected controls object");
        require(inspect(0,at,vt,out)==Read::arguments,"missing object");
        require(inspect(1,1,vt,out)==Read::memory && !out.raw_flags,"invalid memory guarded");
        put(object,0x198,uintptr_t{1});
        require(inspect(at,at,vt,out)==Read::memory && !out.thread,"invalid used source guarded");object=saved;

        Cache cache;Read status{};
        require(!cache.read(out,status,1000) && status==Read::unavailable,"empty cache");
        const auto sample=[&](uint64_t now) {cache.begin();cache.finish(at,at,vt,now);};
        sample(1000);
        require(cache.read(out,status,1000) && status==Read::ok && out.predicate_enabled,"fresh copied cache");
        require(cache.read(out,status,1100),"100ms boundary");
        require(!cache.read(out,status,1101) && status==Read::unavailable && !out.raw_flags,"stale observations clear output");
        require(!cache.read(out,status,999),"backward clock refused");
        cache.begin();require(!cache.read(out,status,1001),"in-flight update invalidates cached permission");
        cache.finish(at,at,vt,1001);
        cache.begin();cache.begin();cache.finish(at,at,vt,1002);
        require(!cache.read(out,status,1002),"overlapping update stays unknown");
        cache.finish(at,at,vt,1003);require(cache.read(out,status,1003) && status==Read::ok,"last update completes observation");
        cache.begin();cache.finish(1,1,vt,1004);
        require(cache.read(out,status,1004) && status==Read::memory && !out.raw_flags,"copy failure replaces previous eligible state");
        require(!start(),"unknown executable must refuse installation");
        crml::compatibility::reviewed_build=true;
        crml::compatibility::select_profile(crml::compatibility::tested_sha);
        require(!start(),"older reviewed executable must not use October addresses");
        crml::compatibility::reviewed_build=false;
        crml::compatibility::select_profile(crml::compatibility::updated_sha);
        require(!start(),"profile selection alone is not build authorization");
        uintptr_t update{},object_address{},predicate_vtable{};
        for(const auto sha:{crml::compatibility::updated_sha,crml::compatibility::hotfix_sha,crml::compatibility::patch_sha}) {
            crml::compatibility::select_profile(sha);
            require(!test_addresses(update,object_address,predicate_vtable) && !update && !object_address && !predicate_vtable,
                    "neither profile grants observation without authorization");
            crml::compatibility::reviewed_build=true;
            const bool hotfix=sha==crml::compatibility::hotfix_sha;
            const bool patch=sha==crml::compatibility::patch_sha;
            require(test_addresses(update,object_address,predicate_vtable) && update==(patch?0x29d5c10:hotfix?0x29d5b60:0x29d5b80) &&
                    object_address==(patch?0x5d0f580:hotfix?0x5df0560:0x5de85b0) && predicate_vtable==(patch?0x4d1db30:hotfix?0x4dc78d0:0x4dbf5b0),
                    "reviewed update, static object and predicate identity must follow the same executable");
            crml::compatibility::reviewed_build=false;
        }
        crml::compatibility::reviewed_build=true;
        for(const auto sha:{crml::compatibility::tested_sha,std::string_view{"unknown"}}) {
            crml::compatibility::select_profile(sha);
            require(!test_addresses(update,object_address,predicate_vtable) && !update && !object_address && !predicate_vtable,
                    "previous and unknown profiles cannot inherit hotfix addresses");
        }
        crml::compatibility::reviewed_build=false;
        require(test_dispatch(),"observer dispatch preserves arguments and engine exception");
        std::cout<<"Input context identity, predicate, generation, read-only copy, expiry and dispatch checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
