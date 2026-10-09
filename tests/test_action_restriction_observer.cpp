#include "diagnostics/action_restriction_observer.h"
#include "diagnostics/action_context_snapshot.h"
#include "diagnostics/applied_story_state.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <limits>
#include <thread>

using namespace crml::action_restriction_observer::testing;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
void applied_state_tests() {
    using namespace crml::action_restriction_observer;
    AppliedStoryState state;
    crml::structural_lifecycle::Snapshot structural{};structural.available=true;structural.sequence=10;
    crml::player_status_lifecycle::Snapshot lifecycle{};lifecycle.available=true;lifecycle.sequence=6;
    StoryReasons reasons{};reasons.counts[0]=1;reasons.active_mask=1;reasons.enabled=true;
    AppliedStoryState::Record record{123,456,0,0,6,0,reasons},out{};
    auto read=[&] {return state.read(123,456,reasons,structural,lifecycle,out);};
    require(!read(),"empty applied state is unknown");
    auto publish=[&] {const auto token=state.begin_write();require(!read(),"in-flight write is unavailable");state.end_write(token,&record);};
    publish();require(read() && state.current(out),"validated manager write retained");
    structural.sequence+=200;structural.flushes+=100;
    require(read(),"ordinary completed flushes do not erase reason provenance");
    structural.active=1;require(!read(),"in-flight structure change rejects");structural.active=0;
    ++structural.teardowns;require(!read(),"world teardown invalidates even if address reappears");--structural.teardowns;
    ++structural.unwinds;require(!read(),"structural unwind rejects");--structural.unwinds;
    ++lifecycle.sequence;require(!read(),"component init or copy invalidates despite identical bytes");--lifecycle.sequence;
    lifecycle.available=false;require(!read(),"partial hook coverage never qualifies");lifecycle.available=true;
    require(!state.read(124,456,reasons,structural,lifecycle,out) && !state.read(123,457,reasons,structural,lifecycle,out),
            "different world or generational player invalidates");
    reasons.counts[2]=1;reasons.active_mask|=4;require(!read(),"new conversation reason rejects old area observation");
    reasons=record.reasons;
    auto token=state.begin_write();state.end_write(token,nullptr);require(!read(),"unknown writer or unwind invalidates");
    publish();
    auto first=state.begin_write(),second=state.begin_write();
    state.end_write(first,&record);state.end_write(second,&record);require(!read(),"overlapping writes cannot publish ambiguous ordering");
    publish();require(read(),"fresh manager decision recovers after rejected overlap");
    state.invalidate();require(!read(),"closed capture and unobserved writes invalidate without sampling");
    publish();require(read(),"fresh observation after explicit invalidation");
    const auto prior=out;token=state.begin_write();require(!state.current(prior),"previous result retires before writer executes");
    state.end_write(token,nullptr);
    std::atomic<bool> finished{},bad{};
    std::thread writer([&] {
        for(unsigned i=0;i<4000;++i) {
            auto next=record;next.reasons.counts[0]=1+(i&1);
            const auto stamp=state.begin_write();state.end_write(stamp,&next);
        }
        finished=true;
    });
    while(!finished.load()) {
        AppliedStoryState::Record value{};
        if(state.read(123,456,reasons,structural,lifecycle,value) &&
           (value.world!=123 || value.entity!=456 || value.reasons.counts!=reasons.counts || !value.writer_sequence)) bad=true;
    }
    writer.join();require(!bad.load(),"concurrent readers never receive a torn publication");
}
template<class Buffer,class Value> void put(Buffer& buffer,size_t offset,Value value) {
    std::memcpy(buffer.data()+offset,&value,sizeof(value));
}
void context_tests() {
    using namespace crml::action_restriction_observer;
    std::vector<unsigned char> world(0x58600),registry(0x60),chunk(0x1000),tags(1024),meta(16);
    std::array<unsigned char,4> slots{};
    std::array<uint32_t,2> hashes{0x9988f341,0x72d38548},offsets{0x80,0x900};
    std::array<uint64_t,1> generations{1},locations{0};
    auto address=[](auto& data) {return reinterpret_cast<uintptr_t>(data.data());};
    put(world,0,address(registry));put(registry,0x48,address(slots));put(registry,0x50,uint32_t{1});
    put(world,0x58478,address(meta));put(meta,8,uint64_t{1});
    put(world,0x58480,address(tags));tags[0]=1;
    put(world,0x50,address(chunk));put(world,0x10448,uint32_t{1});
    put(world,0x584e8,address(generations));put(world,0x58510,uint64_t{1});put(world,0x58530,address(locations));
    put(chunk,0x10,uint64_t{1}<<32);chunk[0x900]=4;chunk[0x901]=1;
    put(world,0x18450,address(hashes));put(world,0x18458,address(offsets));put(world,0x18464,uint32_t{2});
    ActionContext result{};
    require(copy_action_context(address(world),0,result) && result.logic==address(chunk)+0x80 &&
            result.entity==(uint64_t{1}<<32) && result.mode==4 && result.flags==1 &&
            result.status==address(chunk)+0x900,"unique current player and status resolved");
    require(!result.structural_observed,"unavailable observer does not claim coverage");
    auto alternate_world=world; // Same entity/components, different owning world.
    context_fixture(address(world),address(alternate_world));
    const auto writer_context_ok=reason_capture();
    context_fixture(0);
    require(writer_context_ok,"same-call manager write matches synthetic live player through assembly entry");
    crml::structural_lifecycle::testing::arm();
    require(copy_action_context(address(world),0,result) && result.structural_observed,
            "quiet observed interval accepts diagnostic snapshot");
    require(crml::structural_lifecycle::testing::begin_flush(),"synthetic flush admitted");
    require(!copy_action_context(address(world),0,result) && !result.status,
            "active structural work rejects player snapshot");
    crml::structural_lifecycle::testing::end_flush();
    testing::before_status_recheck=+[] {
        crml::structural_lifecycle::testing::begin_flush();
        crml::structural_lifecycle::testing::end_flush();
    };
    require(!copy_action_context(address(world),0,result) && !result.status,
            "structural work during final reads rejects unchanged-looking player");
    testing::before_status_recheck=&crml::structural_lifecycle::stop;
    require(!copy_action_context(address(world),0,result),"capture shutdown crossing rejects coverage");
    testing::before_status_recheck=nullptr;
    require(copy_action_context(address(world),0,result) && !result.structural_observed,
            "stopped capture preserves explicitly unobserved diagnostics");
    generations[0]=2;
    require(!copy_action_context(address(world),0,result) && !result.logic,"stale entity generation rejected");generations[0]=1;
    put(world,0x10448,uint32_t{2});require(!copy_action_context(address(world),0,result),"multiple tagged entities rejected");
    put(world,0x10448,uint32_t{1});put(meta,8,uint64_t{2});tags[0]=3;
    put(world,0x58,address(chunk));put(world,0x1044c,uint32_t{1});
    require(!copy_action_context(address(world),0,result),"two live player archetypes rejected");
    tags[0]=1;put(meta,8,uint64_t{1});hashes[0]=0;
    require(!copy_action_context(address(world),0,result),"missing logic component rejected");hashes[0]=0x9988f341;
    require(!copy_action_context(address(world),1,result) && !copy_action_context(1,0,result),"invalid tag and memory rejected");
    std::array<uint32_t,3> env_hashes{1,0xe7097951,0xffffffff};
    std::array<uintptr_t,3> env_values{0,address(chunk),0};
    put(world,0x585b0,address(env_hashes));put(world,0x585b8,uint32_t{3});put(world,0x585c0,address(env_values));
    require(fact_dictionary(address(world))==address(chunk),"exact fact environment resolved");
    env_hashes[1]=0xe7097950;require(!fact_dictionary(address(world)),"neighbor environment never selected");
    env_hashes[1]=env_hashes[2]=0xe7097951;require(!fact_dictionary(address(world)),"duplicate environment rejected");
    require(!fact_dictionary(1),"invalid world memory rejected");
    std::array<unsigned char,0x80> dictionary{};
    std::array<unsigned char,64*40> buckets{};
    put(dictionary,0x48,uint64_t{64});put(dictionary,0x78,address(buckets));
    put(dictionary,0x50,UINT64_MAX);put(dictionary,0x58,UINT64_MAX);
    for(size_t i=0;i<64;++i) {put(buckets,i*40,UINT64_MAX);put(buckets,i*40+8,UINT64_MAX);}
    std::array<size_t,6> positions{};
    for(size_t i=0;i<story_fact_hashes.size();++i) {
        auto index=story_fact_hashes[i]&63;
        uint64_t step{};
        while(buckets[index*40]!=0xff) {++step;index=(index+step)&63;}
        positions[i]=index*40;
        put(buckets,index*40,uint64_t{0});put(buckets,index*40+8,story_fact_hashes[i]);
        put(buckets,index*40+0x10,uint16_t{1});put(buckets,index*40+0x18,double(i==0 || i==4));
    }
    StoryReasons reasons{};
    require(copy_dictionary_reasons(address(dictionary),reasons) && reasons.active_mask==17 &&
            reasons.counts[0]==1 && reasons.counts[4]==1,"six fact values preserve overlapping reasons");
    put(buckets,positions[0]+0x10,uint16_t{6});
    require(!copy_dictionary_reasons(address(dictionary),reasons) && !reasons.enabled,"deleted fact rejected");
    put(buckets,positions[0]+0x10,uint16_t{3});
    require(!copy_dictionary_reasons(address(dictionary),reasons),"entity fact tag is not a Lua numeric TValue tag");
    put(buckets,positions[0]+0x10,uint16_t{1});
    put(buckets,positions[0]+0x18,std::numeric_limits<double>::infinity());
    require(!copy_dictionary_reasons(address(dictionary),reasons),"invalid fact number rejected");
    put(buckets,positions[0]+0x18,double{1});
    put(dictionary,0x48,uint64_t{63});require(!copy_dictionary_reasons(address(dictionary),reasons),"invalid hash capacity rejected");
    put(dictionary,0x48,uint64_t{64});put(buckets,positions[0],UINT64_MAX);put(buckets,positions[0]+8,UINT64_MAX);
    require(!copy_dictionary_reasons(address(dictionary),reasons),"missing key never treated as zero");
    require(!copy_dictionary_reasons(1,reasons),"invalid fact memory rejected");
}
int main() {
    try {
        context_tests();
        applied_state_tests();
        require(callers(),"only three reviewed callers are admitted");
        require(world_lookup(),"gameplay world selected instead of neighboring global-fact world");
        require(invalid_memory(),"invalid engine addresses are guarded");
        require(callthrough(),"six arguments, result, output byte, stop and native unwind are preserved");
        require(bounds(),"sample and ring budgets are bounded");
        require(reporting(),"JSON uses decimal values and restores stream flags");
        require(reason_capture(),"reason capture verifies manager, arguments, bounds and transparent callthrough");
        require(writer_capture(),"story writer preserves arguments, unwind and bounded read-only reporting");
        require(writer_budget(),"startup burst cannot exhaust later writer capture; unsampled writes still run");
        require(native_adapter(),"exact native edge forwards off-scope pairs and tracking survives diagnostic stop");
        require(native_positive(),"verified player and area reasons skip only exact indexed entry and reject intervening Status copy");
        std::cout<<"Action restriction observer tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
