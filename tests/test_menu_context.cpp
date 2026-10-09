#include "diagnostics/menu_context.h"
#include "compatibility.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace crml::menu_context {bool test_addresses(uintptr_t&,uintptr_t&) noexcept;}
using namespace crml::menu_context;
void require(bool yes,const char* why) {if(!yes) throw std::runtime_error(why);}
int main() {
    try {
        std::array<uint8_t,2> bytes{};
        const auto first=reinterpret_cast<uintptr_t>(&bytes[0]),last=reinterpret_cast<uintptr_t>(&bytes[1]);
        constexpr uintptr_t caller=0x12345678;
        Snapshot out{};Read status{};
        require(prepare(caller,caller,first,last)==Read::ok,"reviewed zero-initialized outputs");
        require(prepare(caller+1,caller,1,2)==Read::caller,"foreign caller must not inspect output memory");
        require(prepare(caller,0,first,last)==Read::caller,"missing reviewed caller");
        require(prepare(caller,caller,0,last)==Read::arguments,"missing output");
        require(prepare(caller,caller,first,first)==Read::arguments,"aliased output bytes refused");
        require(prepare(caller,caller,1,2)==Read::memory,"invalid initial memory guarded");
        bytes[1]=0xff;
        require(prepare(caller,caller,first,last)==Read::initial_output,"nonzero second initial output refused");
        bytes={1,0};
        require(prepare(caller,caller,first,last)==Read::initial_output,"nonzero first initial output refused");
        bytes={0,0};
        require(inspect(first,last,out)==Read::ok && !out.matched_active_context && !out.last_match_flag_5c &&
                out.thread==GetCurrentThreadId(),"completed negative scan is copied");
        bytes={1,0xe7};const auto saved=bytes;
        require(inspect(first,last,out)==Read::ok && out.matched_active_context && out.last_match_flag_5c==0xe7,
                "last match detail must remain a raw byte");
        require(bytes==saved,"observer must not modify outputs");
        bytes={2,0xe7};
        require(inspect(first,last,out)==Read::output_value && !out.thread && !out.matched_active_context &&
                !out.last_match_flag_5c,"invalid match Boolean clears the complete snapshot");
        require(inspect(1,2,out)==Read::memory && !out.thread,"completed copy access violation guarded");
        require(inspect(first,0,out)==Read::arguments,"missing completed output");

        Cache cache;
        require(!cache.read(out,status,1000) && status==Read::unavailable,"empty cache");
        bytes={1,0x80};
        auto token=cache.begin();cache.finish(token,Read::ok,first,last,1000);
        require(cache.read(out,status,1000) && status==Read::ok && out.last_match_flag_5c==0x80,"completed cache");
        bytes={0,0};
        require(cache.read(out,status,1100) && out.matched_active_context && out.last_match_flag_5c==0x80,
                "100ms boundary copies values rather than retaining outputs");
        require(!cache.read(out,status,1101) && status==Read::stale && !out.thread,"stale result becomes unknown");
        require(!cache.read(out,status,999) && status==Read::stale,"backward time refused");
        token=cache.begin();
        require(!cache.read(out,status,1001) && status==Read::in_flight,"new scan invalidates previous completion");
        cache.finish(token,Read::ok,first,last,1001);
        require(cache.read(out,status,1001) && status==Read::ok && !out.matched_active_context,"normal negative completion");
        token=cache.begin();cache.finish(token,Read::caller,1,2,1002);
        require(cache.read(out,status,1002) && status==Read::caller && !out.thread,"rejected contract never dereferences outputs");
        token=cache.begin();cache.finish(token,Read::ok,1,2,1003);
        require(cache.read(out,status,1003) && status==Read::memory && !out.thread,"bad copy replaces previous completion");

        auto outer=cache.begin(),inner=cache.begin();
        cache.finish(inner,Read::ok,first,last,1004);
        require(!cache.read(out,status,1004) && status==Read::in_flight,"nested scan stays unknown");
        cache.finish(outer,Read::ok,first,last,1005);
        require(!cache.read(out,status,1005) && status==Read::overlap,"nested last return cannot publish a scan");
        outer=cache.begin();inner=cache.begin();
        cache.finish(outer,Read::ok,first,last,1006);cache.finish(inner,Read::ok,first,last,1007);
        require(!cache.read(out,status,1007) && status==Read::overlap,"opposite overlap return order also remains unknown");
        token=cache.begin();cache.finish(token,Read::ok,first,last,1008);
        require(cache.read(out,status,1008) && status==Read::ok,"later isolated scan can recover after overlap");

        auto* borrowed=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        require(borrowed!=nullptr,"test output allocation");
        borrowed[0]=1;borrowed[1]=0xfe;
        token=cache.begin();
        cache.finish(token,Read::ok,reinterpret_cast<uintptr_t>(borrowed),reinterpret_cast<uintptr_t>(borrowed+1),1009);
        require(VirtualFree(borrowed,0,MEM_RELEASE)!=0,"test output retirement");
        require(cache.read(out,status,1010) && status==Read::ok && out.matched_active_context && out.last_match_flag_5c==0xfe,
                "retired borrowed outputs are never followed by reader");
        cache.begin(); // An unwind intentionally never reaches finish.
        require(!cache.read(out,status,1010) && status==Read::in_flight,"unwound invocation cannot expose old result");
        token=cache.begin();cache.finish(token,Read::ok,first,last,1011);
        require(!cache.read(out,status,1011) && status==Read::in_flight,"later scan cannot rehabilitate unfinished engine call");

        require(!start(),"unknown executable must refuse installation");
        crml::compatibility::reviewed_build=true;
        crml::compatibility::select_profile(crml::compatibility::tested_sha);
        require(!start(),"older reviewed executable must not use October helper addresses");
        crml::compatibility::reviewed_build=false;
        crml::compatibility::select_profile(crml::compatibility::updated_sha);
        require(!start(),"October profile without build authorization cannot install");
        uintptr_t scan{},mapped_caller{};
        for(const auto sha:{crml::compatibility::updated_sha,crml::compatibility::hotfix_sha,crml::compatibility::patch_sha}) {
            crml::compatibility::select_profile(sha);
            require(!test_addresses(scan,mapped_caller) && !scan && !mapped_caller,"both profiles require build authorization");
            crml::compatibility::reviewed_build=true;
            require(test_addresses(scan,mapped_caller) && scan==0x17e2bb0 && mapped_caller==0x17e2842,
                    "reviewed menu helper and borrowed-output caller preserved for both profiles");
            crml::compatibility::reviewed_build=false;
        }
        crml::compatibility::reviewed_build=true;
        for(const auto sha:{crml::compatibility::tested_sha,std::string_view{"unknown"}}) {
            crml::compatibility::select_profile(sha);
            require(!test_addresses(scan,mapped_caller) && !scan && !mapped_caller,"previous and unknown builds cannot inherit menu addresses");
        }
        crml::compatibility::reviewed_build=false;
        require(std::strcmp(name(Read::caller),"unreviewed_caller")==0 &&
                std::strcmp(name(Read::overlap),"overlap")==0,"named diagnostics");
        require(test_dispatch(),"four arguments, outputs and engine exceptions must be preserved");
        std::cout<<"Menu context borrowed outputs, caller contract, expiry, overlap and dispatch checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
