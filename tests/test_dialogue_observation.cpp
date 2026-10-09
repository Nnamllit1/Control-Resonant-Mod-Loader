#include "dialogue_observation.h"
#include "diagnostics/dialogue_observer.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace crml;
namespace {
void require(bool pass,const char* what) {if(!pass) throw std::runtime_error(what);}
template<class T,size_t N> void put(std::array<unsigned char,N>& row,size_t offset,T value) {
    std::memcpy(row.data()+offset,&value,sizeof(value));
}
}
int main() {
    try {
        std::array<unsigned char,0x58> ui{};
        std::array<unsigned char,0x70> source{};
        put(ui,0,uint32_t{73});put(ui,0x50,uint32_t{2});put(ui,0x54,uint8_t{1});
        put(ui,0x40,uint64_t{5});put(ui,0x48,uint64_t{15});
        std::memcpy(ui.data()+0x30,"hello",6);
        put(source,0x4c,uint32_t{73});put(source,0x60,uint32_t{900});
        put(source,0x50,5.0f);put(source,0x54,2.0f);
        dialogue::Observation selected{};
        require(dialogue::inspect_selected(ui.data(),source.data(),selected)==dialogue::Read::ok,
            "selected segment decodes");
        require(selected.allocation==900 && selected.playback_key==73 && selected.segment==2 &&
            selected.length==5 && selected.forced==1 && std::strcmp(selected.text,"hello")==0,
            "selected copy has distinct allocation, playback and segment IDs");
        const auto hash=dialogue::text_hash(selected);
        require(hash!=0,"selected text hashes");
        put(ui,0x50,uint32_t{3});
        dialogue::Observation repeated{};
        require(dialogue::inspect_selected(ui.data(),source.data(),repeated)==dialogue::Read::ok &&
            repeated.segment==3 && dialogue::text_hash(repeated)==hash,
            "identical text in another segment retains distinct identity");
        put(source,0x60,uint32_t{901});
        require(dialogue::inspect_selected(ui.data(),source.data(),repeated)==dialogue::Read::ok &&
            repeated.allocation==901,"new source allocation is distinct despite same text and playback key");
        put(source,0x4c,uint32_t{74});
        require(dialogue::inspect_selected(ui.data(),source.data(),selected)==dialogue::Read::identity &&
            selected.length==0,"mismatched source is never published");
        put(source,0x4c,uint32_t{73});
        put(ui,0x30,uint8_t{0xc0});
        require(dialogue::inspect_selected(ui.data(),source.data(),selected)==dialogue::Read::encoding,
            "invalid UTF-8 is rejected");
        std::memcpy(ui.data()+0x30,"hello",6);
        put(ui,0x40,uint64_t{4097});put(ui,0x48,uint64_t{4097});
        require(dialogue::inspect_selected(ui.data(),source.data(),selected)==dialogue::Read::range,
            "overlong text is bounded before pointer traversal");
        put(ui,0x40,uint64_t{5});put(ui,0x48,uint64_t{15});
        require(dialogue::inspect_selected(reinterpret_cast<void*>(1),source.data(),selected)==dialogue::Read::memory,
            "unreadable native row is contained");
        require(dialogue::inspect_selected(nullptr,source.data(),selected)==dialogue::Read::arguments,
            "missing native row is rejected");
        std::array<unsigned char,0x18> state{};
        put(state,0,reinterpret_cast<uintptr_t>(ui.data()));put(state,8,uint32_t{1});
        require(dialogue::inspect_published(state.data(),0,selected)==dialogue::Read::ok &&
            selected.segment==3,"published slot copies only current row");
        require(dialogue::inspect_published(state.data(),1,selected)==dialogue::Read::empty,
            "absent published slot stays empty");
        put(state,8,uint32_t{65});
        require(dialogue::inspect_published(state.data(),0,selected)==dialogue::Read::range,
            "malformed vector size is rejected");

        dialogue_observer::Admissions gate;
        require(!gate.enter(),"admission begins closed");
        gate.open();require(gate.enter(),"admission opens");gate.close();
        require(!gate.enter() && gate.pending()==1,"closure blocks new work and preserves pending work");
        gate.leave();require(!gate.pending(),"pending work drains");
        dialogue_observer::Recorder recorder;
        dialogue_observer::Event event{};
        event.sequence=1;event.time_ms=100;event.read=dialogue::Read::ok;
        event.allocation=900;event.playback_key=73;event.segment=3;
        event.text_length=5;event.text_hash=hash;
        recorder.begin();recorder.end(true,true,&event);
        dialogue_observer::Event drained{};
        require(recorder.drain(&drained,1)==1 && drained.text_hash==hash && drained.segment==3,
            "recorder preserves copied metadata without engine pointers or text");
        recorder.begin();recorder.end(false,false,nullptr);
        for(unsigned i=0;i<300;++i) {event.sequence=i+2;recorder.begin();recorder.end(true,true,&event);}
        std::array<dialogue_observer::Event,256> batch{};
        const auto count=recorder.drain(batch.data(),batch.size());
        const auto totals=recorder.counts();
        require(count==256 && totals.calls==302 && totals.changes==301 && totals.samples==301 &&
            totals.dropped==44 && totals.unwinds==1 && totals.in_flight==0,
            "bounded ring explicitly counts overflow and native unwinds");
        require(!recorder.drain(batch.data(),batch.size()),"ring drains exactly once");
        require(dialogue_observer::test_callers(),"only the reviewed worker caller is accepted");
        require(dialogue_observer::test_callthrough(),"worker arguments and return survive selection and foreign caller rejection");
        require(dialogue_observer::test_native_unwind(),"native engine exception propagates and closes observer admission");
        require(dialogue_observer::test_reporting(),"JSON reporting preserves 64-bit hash and restores stream format");
        std::cout<<"Dialogue observation tests passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
