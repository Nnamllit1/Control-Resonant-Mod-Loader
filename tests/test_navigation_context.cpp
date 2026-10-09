#include "navigation_context.h"
#include "diagnostics/navigation_observer.h"
#include "compatibility.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace crml;
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
template<class T,size_t N> void put(std::array<unsigned char,N>& at,size_t offset,T value) {
    std::memcpy(at.data()+offset,&value,sizeof(value));
}
int main() {
    try {
        using namespace navigation_context;
        std::array<unsigned char,0xa0> state{};
        std::array<unsigned char,0x110> view{};
        std::array<unsigned char,96> transforms{};
        std::array<unsigned char,40> saved{};
        const auto state_address=reinterpret_cast<uintptr_t>(state.data());
        const auto transform_address=reinterpret_cast<uintptr_t>(transforms.data());
        uintptr_t smart_slot=state_address;
        uintptr_t loading_slot=state_address;
        put(view,0x108,reinterpret_cast<uintptr_t>(&smart_slot));
        put(view,0x78,transform_address);
        put(view,0x88,uint64_t{2});
        for(unsigned i=0;i<8;++i) put(transforms,64+i*4,float(i+1));
        put(state,0x90,uint64_t{0xfedcba9876543210});
        put(state,0x98,uint8_t{1});
        Bundle bundle{};SavedTransform source{};
        using namespace compatibility;
        reviewed_build=false;engine_profile=EngineProfile::october_patch;
        require(inspect_bundle(&loading_slot,bundle)==Status::unavailable,"unreviewed profile accepted");
        reviewed_build=true;engine_profile=EngineProfile::october_update;
        require(inspect_save_source(view.data(),source)==Status::unavailable,"older profile accepted");
        engine_profile=EngineProfile::october_patch;
        require(inspect_bundle(&loading_slot,bundle)==Status::ok && bundle.valid && bundle.value==0xfedcba9876543210,"loading optional bundle");
        require(inspect_save_source(view.data(),source)==Status::ok && source.bundle_valid && source.bundle==bundle.value && source.transform[0]==1 && source.transform[7]==8,"save view double pointer and nonzero row");
        std::memcpy(saved.data(),transforms.data()+64,32);put(saved,32,source.bundle);
        require(inspect_save_result(saved.data(),source)==Status::ok,"saved value matches source");
        put(saved,32,uint64_t{2});require(inspect_save_result(saved.data(),source)==Status::mismatch,"mismatched result accepted");
        put(state,0x98,uint8_t{0});
        require(inspect_bundle(&loading_slot,bundle)==Status::ok && !bundle.valid && !bundle.value,"absent optional bundle");
        require(inspect_save_source(view.data(),source)==Status::ok && !source.bundle_valid && !source.bundle,"absent save bundle must write zero");
        put(state,0x98,uint8_t{2});
        require(inspect_bundle(&loading_slot,bundle)==Status::malformed && !bundle.valid && !bundle.value,"invalid optional flag accepted");
        put(state,0x98,uint8_t{1});
        smart_slot=1;
        require(inspect_save_source(view.data(),source)==Status::memory && !source.bundle_valid,"unreadable GameState accepted");
        smart_slot=state_address;
        put(view,0x88,uint64_t{65536});
        require(inspect_save_source(view.data(),source)==Status::arguments,"unbounded row accepted");
        put(view,0x88,uint64_t{2});put(transforms,64,float{NAN});
        require(inspect_save_source(view.data(),source)==Status::malformed && !source.bundle_valid,"nonfinite transform accepted");
        require(inspect_bundle(reinterpret_cast<void*>(1),bundle)==Status::memory,"unreadable loading slot accepted");
        std::array<unsigned char,0xa0> restore_state{};
        std::array<unsigned char,0x211> restore_view{};
        std::array<unsigned char,40> restore_saved{};
        uintptr_t restore_slot=reinterpret_cast<uintptr_t>(restore_state.data());
        put(restore_view,0xd8,reinterpret_cast<uintptr_t>(&restore_slot));
        constexpr uint64_t high_bundle=0xfedcba9876543210;
        put(restore_state,0x90,high_bundle);put(restore_state,0x98,uint8_t{1});
        put(restore_saved,32,high_bundle);
        for(unsigned i=0;i<8;++i) put(restore_saved,i*4,float(i+1));
        RestoreObservation restore{};
        reviewed_build=false;
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::unavailable,
            "unreviewed restore profile accepted");
        reviewed_build=true;engine_profile=EngineProfile::october_update;
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::unavailable,
            "older restore profile accepted");
        engine_profile=EngineProfile::october_patch;
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.reason==RestoreReason::eligible && !restore.context_disabled &&
            restore.saved.bundle_valid && restore.saved.bundle==high_bundle &&
            restore.current.valid && restore.current.value==high_bundle &&
            restore.saved.transform[0]==1 && restore.saved.transform[7]==8,
            "eligible restore inputs were not copied");
        require(std::string{name(RestoreReason::eligible)}=="eligible","restore reason name missing");
        put(restore_view,0x210,uint8_t{1});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.context_disabled && restore.reason==RestoreReason::context_disabled,
            "disabled restore context accepted");
        put(restore_view,0x210,uint8_t{2});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::malformed &&
            !restore.saved.bundle_valid && !restore.current.valid,"invalid restore gate accepted");
        put(restore_view,0x210,uint8_t{0});put(restore_saved,32,uint64_t{0});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.reason==RestoreReason::missing_saved_bundle && !restore.saved.bundle_valid,
            "zero saved bundle was not rejected");
        put(restore_saved,32,high_bundle);put(restore_state,0x98,uint8_t{0});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.reason==RestoreReason::missing_current_bundle && !restore.current.valid && !restore.current.value,
            "absent current optional bundle was not rejected");
        put(restore_state,0x98,uint8_t{1});put(restore_state,0x90,uint64_t{0});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.reason==RestoreReason::bundle_mismatch && restore.current.valid && !restore.current.value,
            "present zero current bundle was confused with absent optional");
        put(restore_state,0x90,uint64_t{0x0000000076543210});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::ok &&
            restore.reason==RestoreReason::bundle_mismatch,
            "restore bundle comparison discarded high bits");
        put(restore_state,0x98,uint8_t{2});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::malformed &&
            !restore.saved.bundle_valid,"invalid current optional flag accepted");
        put(restore_state,0x98,uint8_t{1});
        require(inspect_restore_source(reinterpret_cast<void*>(1),restore_view.data(),restore)==Status::memory,
            "unreadable saved restore record accepted");
        require(inspect_restore_source(restore_saved.data(),reinterpret_cast<void*>(1),restore)==Status::memory,
            "unreadable restore view accepted");
        restore_slot=1;
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::memory,
            "unreadable restore GameState accepted");
        restore_slot=reinterpret_cast<uintptr_t>(restore_state.data());
        require(inspect_restore_source(nullptr,restore_view.data(),restore)==Status::arguments,
            "null restore record accepted");
        put(restore_saved,0,float{NAN});
        require(inspect_restore_source(restore_saved.data(),restore_view.data(),restore)==Status::malformed &&
            !restore.saved.bundle_valid,"nonfinite restore transform accepted");
        require(navigation_observer::testing::gate(),"closed admission allowed late entry");
        require(navigation_observer::testing::callers(),"foreign caller read memory");
        require(navigation_observer::testing::callthrough(),"hook forwarding or native unwind lost admission");
        std::ostringstream log;log<<std::hex<<std::showpos<<std::boolalpha<<std::setprecision(3);
        navigation_observer::testing::emit(log);
        const auto text=log.str();
        require(text.find("Capability location: {\"schema\":1,\"type\":\"sample\"")!=std::string::npos,"sample envelope missing");
        require(text.find("\"before_bundle\":\"0\"")!=std::string::npos &&
            text.find("\"after_bundle\":\"18446744073709551615\"")!=std::string::npos,"uint64 identifiers lost precision");
        require(text.find("\"saveposition\":[1.25,-2,3]")!=std::string::npos &&
            text.find("\"matched\":true")!=std::string::npos,"validated save coordinate missing");
        const auto restore_phase=text.find("\"phase\":\"restore_coordinate\"");
        require(restore_phase!=std::string::npos,"restore coordinate sample missing");
        const auto restore_start=text.rfind("Capability location: ",restore_phase);
        const auto restore_end=text.find('\n',restore_phase);
        require(restore_start!=std::string::npos && restore_end!=std::string::npos,
            "restore sample line missing");
        const auto restore_line=text.substr(restore_start,restore_end-restore_start);
        require(restore_line.find("\"restore_reason\":\"bundle_mismatch\"")!=std::string::npos &&
            restore_line.find("\"saved_bundle\":\"18446744073709551615\"")!=std::string::npos &&
            restore_line.find("\"returned\":true")!=std::string::npos,
            "restore input fields or uint64 identifier missing");
        require(restore_line.find("\"after_bundle\"")==std::string::npos &&
            restore_line.find("\"matched\"")==std::string::npos &&
            restore_line.find("\"saveposition\"")==std::string::npos,
            "restore sample claimed a post-call transform observation");
        require(text.find("\"type\":\"totals\"")!=std::string::npos,"totals envelope missing");
        require(text.find("\"sequence\":42")!=std::string::npos &&
            text.find("\"time_ms\":123")!=std::string::npos,"JSON numbers inherited caller stream flags");
        require((log.flags()&std::ios::basefield)==std::ios::hex &&
            (log.flags()&std::ios::showpos)!=0 && log.precision()==3,
            "serializer did not restore caller stream formatting");
        std::cout<<"Navigation context and observer checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
