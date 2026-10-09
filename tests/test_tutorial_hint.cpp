#include "tutorial_hint.h"
#include "mod_tutorials.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
using namespace crml;
using namespace crml::tutorial;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class T> T get(const void* p,size_t offset=0) {T v{};std::memcpy(&v,static_cast<const char*>(p)+offset,sizeof(v));return v;}
template<class T> void set(void* p,size_t offset,T v) {std::memcpy(static_cast<char*>(p)+offset,&v,sizeof(v));}
std::map<void*,std::string> source_strings;
std::string built_title,built_body;
unsigned built{},destroyed{},game_calls{},private_calls{};
unsigned game_input_calls{},private_input_calls{};
bool simulate_press{},fault_input{};
uint32_t expected_input_mode{};
float observed_duration{},observed_elapsed{};
bool fault_draw{};
unsigned char* last_page{};
void* assign(void* destination,const NativeStringView* value) {
    source_strings[destination]=std::string(value->data,value->size);return destination;
}
void destroy_string(void* p) {source_strings.erase(p);}
PageVector* build(PageVector* pages,const PageSource* source) {
    require(source->count==1,"one page source");
    built_title=source_strings[static_cast<char*>(const_cast<void*>(source->data))+0x10];
    built_body=source_strings[static_cast<char*>(const_cast<void*>(source->data))+0x38];
    last_page=new unsigned char[0x88]{};pages->data=last_page;pages->count=pages->capacity=1;++built;return pages;
}
void destroy(PageVector* pages) {delete[] static_cast<unsigned char*>(pages->data);last_page=nullptr;*pages={};++destroyed;}
std::array<unsigned char,0x70> game_data{};
std::array<unsigned char,0x38> game_state{};
std::array<uintptr_t,4> game_query{};
void worker(const void* query,void* state,const void*,const void*,void*) {
    const auto* data=reinterpret_cast<const unsigned char*>(get<uintptr_t>(query));
    if(data==game_data.data()) {++game_calls;return;}
    ++private_calls;
    if(fault_draw) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    require(get<uint64_t>(query,16)==0,"private query has row zero");
    require(state!=game_state.data(),"native game state never reused for mod");
    const auto* controls=reinterpret_cast<const unsigned char*>(get<uintptr_t>(data,8));
    const auto* slots=reinterpret_cast<const unsigned char*>(get<uintptr_t>(data,0x10));
    require(get<uint64_t>(data,0x20)==31 && controls[31]==0xff,"native sentinel and mask");
    constexpr uint64_t hash=0xde5fb9d2630458e9ULL;
    const auto slot=(hash>>7)&31;
    require(controls[slot]==(hash&0x7f) && get<uint32_t>(slots,slot*0x68)==1,"native lookup resolves private key");
    require(get<uintptr_t>(slots,slot*0x68+8)!=0 && get<uint32_t>(slots,slot*0x68+16)==1,"native record owns exactly one page");
    const auto value=slot*0x68+8; // Native lookup returns the value, not slot header.
    require(get<uint32_t>(slots,value+0x18)==expected_input_mode,"native dismissal mode");
    observed_duration=get<float>(slots,value+0x48);
    observed_elapsed=get<float>(slots,value+0x4c);
    require(get<uint32_t>(slots,slot*0x68+0x18)==0,"header-relative input write would miss the native value");
    require(get<float>(slots,slot*0x68+0x48)==0,"header-relative timer write would miss the native value");
    const auto phase=get<uint32_t>(state,4);
    switch(phase) {
        case 0: set(state,0,data[4]?uint32_t{1}:uint32_t{});if(data[4]) set(state,4,uint32_t{1});break;
        case 1: set(state,4,uint32_t{2});break;
        case 2: if(!data[4]) set(state,4,uint32_t{3});break;
        case 3: set(state,4,uint32_t{});break;
        default: require(false,"valid native state");
    }
}
void input_worker(const void* query,const void* displayed,const void*) {
    const auto* data=reinterpret_cast<const unsigned char*>(get<uintptr_t>(query));
    if(data==game_data.data()) {++game_input_calls;return;}
    ++private_input_calls;
    if(fault_input) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    require(get<uint64_t>(query,24)==0,"private input query row zero");
    require(get<uint32_t>(displayed)==1,"private displayed identifier");
    const auto* controls=reinterpret_cast<const unsigned char*>(get<uintptr_t>(data,8));
    const auto* slots=reinterpret_cast<const unsigned char*>(get<uintptr_t>(data,0x10));
    constexpr uint64_t hash=0xde5fb9d2630458e9ULL;
    const auto slot=(hash>>7)&31;
    require(controls[slot]==(hash&0x7f) && get<uint32_t>(slots,slot*0x68)==1 &&
        get<uint32_t>(slots,slot*0x68+8+0x18)==1,"native input reads value-relative dismiss mode");
    auto* vector=reinterpret_cast<unsigned char*>(get<uintptr_t>(query,8));
    require(get<uint32_t>(vector,8)==0 && get<uint32_t>(vector,12)==1,"bounded private request vector");
    if(simulate_press) {
        auto* record=reinterpret_cast<unsigned char*>(get<uintptr_t>(vector));
        set(record,0,uint8_t{3});set(record,4,uint32_t{1});set(vector,8,uint32_t{1});
    }
}
struct Fixture {
    ModTutorials service;
    explicit Fixture(bool prompt=false) {
        game_data.fill(0);game_state.fill(0);game_query={reinterpret_cast<uintptr_t>(game_data.data()),0,0,0};
        built=destroyed=game_calls=private_calls=game_input_calls=private_input_calls=0;
        fault_draw=simulate_press=fault_input=false;expected_input_mode=prompt?1:0;
        observed_duration=observed_elapsed=0;source_strings.clear();
        PayloadNative native{0,assign,destroy_string,build,destroy};
        hint_test_initialize(worker,native,service,prompt?input_worker:nullptr);require(service.attach(1),"attach owner");
    }
    uint64_t show(std::string_view title="Heading",std::string_view body="Message") {
        const auto ticket=service.show(1,CRML_TUTORIAL_HINT,title,body,1000);require(ticket>0,"request accepted");return ticket;
    }
    void tick(uint64_t now) {hint_test_sync(game_query.data(),game_state.data(),nullptr,now);}
    void input() {hint_test_input(game_query.data(),game_state.data(),nullptr);}
};
void prompt_native_input_and_progress() {
    Fixture f(true);
    const auto ticket=f.service.show(1,CRML_TUTORIAL_PROMPT,"Heading","Message",2000);
    require(ticket>0,"prompt request accepted");
    f.tick(100);
    require(f.service.status(1,ticket)==CRML_TUTORIAL_PRESENTED,"prompt entered native state");
    // The private worker inspects dismiss mode; the model uses duration and
    // elapsed floats from the same private record at +0x48/+0x4c.
    f.tick(600);
    require(private_calls==2 && game_calls==2,"prompt stays on private model");
    require(observed_duration==2.0f && observed_elapsed==0.5f,"stock timer progress follows host duration");
    simulate_press=true;f.input();
    require(game_input_calls==1 && private_input_calls==1,"original input forwarded and private input evaluated");
    require(f.service.status(1,ticket)==CRML_TUTORIAL_PRESENTED,"input event waits for native fade");
    f.tick(610);f.tick(620);f.tick(630);f.tick(640);
    require(f.service.status(1,ticket)==CRML_TUTORIAL_DISMISSED && destroyed==1,
        "native input dismisses only after private state retires");
    require(game_data[4]==0 && get<uint32_t>(game_state.data())==0,"game tutorial state untouched");
}
void prompt_input_guard_and_fault() {
    Fixture f(true);
    const auto ticket=f.service.show(1,CRML_TUTORIAL_PROMPT,"Heading","Message",2000);
    f.tick(100);
    game_data[4]=1;f.input();
    require(game_input_calls==1 && private_input_calls==0,"game hint preempts private input");
    game_data[4]=0;fault_input=true;f.input();
    require(!f.service.available(CRML_TUTORIAL_PROMPT) &&
        f.service.status(1,ticket)==CRML_TUTORIAL_FAILED,
        "fault disables prompt without acknowledging native dismissal");
    // Test fake holds no native references; production quarantines uncertainty.
    delete[] last_page;last_page=nullptr;
}
void prompt_options_disable_native_controls() {
    Fixture f(true);
    expected_input_mode=0;
    const auto ticket=f.service.show(1,CRML_TUTORIAL_PROMPT,"Heading","Message",1000,{}, {},0);
    require(ticket>0,"options-free timed prompt accepted");
    f.tick(100);f.tick(300);
    require(observed_duration==0 && observed_elapsed==0.2f,"progress option controls native timer model");
    simulate_press=true;f.input();
    require(private_input_calls==0,"native dismissal option controls input worker");
    f.tick(1100);f.tick(1200);f.tick(1300);f.tick(1400);
    require(f.service.status(1,ticket)==CRML_TUTORIAL_DISMISSED,"options-free prompt still times out");
}
void duration_and_text() {
    Fixture f;auto ticket=f.show("Heading","<script>&\"'\nplain");
    f.tick(100);require(f.service.status(1,ticket)==CRML_TUTORIAL_PRESENTED,"native submission observed");
    require(built_title=="Heading" && built_body=="&lt;script&gt;&amp;&quot;&#39;<br>plain","plain text escaped once without injected missing glyphs");
    require(source_strings.empty(),"source temporaries destroyed");
    const auto unchanged_data=game_data;
    const auto unchanged_state=game_state;
    f.tick(200);f.tick(1100);require(destroyed==0,"fade does not free pages");
    f.tick(1200);require(destroyed==0,"hidden state with retained ID does not free pages");
    f.tick(1300);require(destroyed==1 && !hint_test_has_payload(),"later cleared ID retires private page");
    require(f.service.status(1,ticket)==CRML_TUTORIAL_DISMISSED,"duration completes after retirement");
    require(game_data==unchanged_data && game_state==unchanged_state && game_calls==5,"game data and lifecycle forwarded intact");
}
void cancellation_and_preemption() {
    Fixture f;const auto ticket=f.show();
    game_data[4]=1;set(game_data.data(),0,uint32_t{1});f.tick(10);
    require(built==0 && f.service.status(1,ticket)==CRML_TUTORIAL_QUEUED,"native selection wins even with same identifier");
    game_data[4]=0;f.tick(100);f.tick(200);
    set(game_state.data(),4,uint32_t{2});const auto calls=private_calls;f.tick(300);
    require(private_calls==calls,"active native hint preempts private renderer");
    set(game_state.data(),4,uint32_t{});f.tick(400);
    require(private_calls==calls+1,"private hint refreshes from initial state after preemption");
    require(f.service.dismiss(1,ticket)==1,"request cancellation");f.tick(410);f.tick(420);f.tick(430);
    require(destroyed==0 && f.service.status(1,ticket)==CRML_TUTORIAL_CANCELLING,"cancel waits through fade and stale ID");
    f.tick(440);require(destroyed==1 && f.service.status(1,ticket)==CRML_TUTORIAL_CANCELLED,"cancel acknowledged after clear");
}
void reload_and_busy_retirement() {
    Fixture f;auto ticket=f.show();f.tick(100);
    f.service.detach(1);game_data[4]=1;f.tick(110);
    require(destroyed==1 && !hint_test_has_payload(),"reload frees private page once native hint replaces it");
    require(f.service.attach(1),"owner can reattach after retirement");
    game_data[4]=0;ticket=f.show();f.tick(120);f.tick(130);
    f.service.cancel(1);f.tick(140);f.tick(150);f.tick(160);
    require(f.service.status(1,ticket)==CRML_TUTORIAL_CANCELLED && destroyed==2,"new generation cancels independently");
}
void bad_query_and_native_fault() {
    Fixture f;auto ticket=f.show();game_query[2]=16384;f.tick(10);
    require(built==0,"out of range query refused after original forward");game_query[2]=0;
    fault_draw=true;f.tick(20);
    require(!hint_available() && !f.service.available(CRML_TUTORIAL_HINT),"native fault disables backend");
    require(f.service.status(1,ticket)==CRML_TUTORIAL_FAILED && destroyed==0,"uncertain native operation not acknowledged as disposal");
    const auto calls=private_calls;f.tick(30);require(private_calls==calls && built==1,"fault cannot accumulate allocations");
    // Test-only reclamation: the fake worker kept no references. Production
    // deliberately retains this bounded allocation after uncertain native fault.
    delete[] last_page;last_page=nullptr;
}
}
int main() {
    try {duration_and_text();cancellation_and_preemption();reload_and_busy_retirement();bad_query_and_native_fault();
        prompt_native_input_and_progress();prompt_input_guard_and_fault();prompt_options_disable_native_controls();}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"tutorial hint lifecycle checks passed\n";
}
