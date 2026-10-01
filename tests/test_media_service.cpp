#include "media_service.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(value) do {if(!(value)) throw std::runtime_error("Media service check failed at line "+std::to_string(__LINE__)+": " #value);} while(false)
constexpr uint32_t mapped_name=CRML_MEDIA_MAPPED_NAME;
struct Fixture {
    crml::media::Service service;
    uintptr_t identity{0x1000};
    uint64_t now{100};
    Fixture() {service.enable(true);}
    bool observe(bool active=true,bool ready=true,uint32_t elapsed=2001,
                 std::string_view name="media/boot-logo.bk2",uint32_t source=CRML_MEDIA_ENGINE_NAME,
                 bool consume=false) {
        return service.observe_at(identity,active,ready,elapsed,name,source,now,consume);
    }
    crml_media_state read() {
        crml_media_state out{};CHECK(service.read_at(out,now)==1);return out;
    }
    int request(uint64_t owner=7) {return service.request_at(owner,read().generation,now);}
};
void unavailable_and_guard() {
    crml::media::Service disabled;crml_media_state out;
    std::memset(&out,0x7f,sizeof(out));CHECK(disabled.read_at(out,100)<0);
    const crml_media_state zero{};CHECK(std::memcmp(&out,&zero,sizeof(out))==0);
    CHECK(disabled.request_at(7,1,100)<0);
    CHECK(!disabled.observe_at(1,true,true,3000,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,100,true));
    Fixture f;CHECK(!f.observe(true,false,3000));out=f.read();
    CHECK(out.size==CRML_MEDIA_STATE_SIZE && out.version==CRML_MEDIA_STATE_VERSION && out.reserved==0);
    CHECK((out.flags&CRML_MEDIA_ACTIVE)!=0 && (out.flags&CRML_MEDIA_SKIPPABLE)==0);
    CHECK(f.request()<0);
    CHECK(!f.observe(true,true,1999));CHECK(f.request()<0);
    CHECK(!f.observe(true,true,2000));CHECK(f.request()<0); // Strictly greater, not >=.
    CHECK(!f.observe(true,true,2001));out=f.read();
    CHECK((out.flags&CRML_MEDIA_SKIPPABLE)!=0 && out.elapsed_ms==2001);
    CHECK(out.generation!=0 && f.request()==0);
    // Merely observing on a read path cannot consume the pending operation.
    CHECK(!f.observe());CHECK(f.request(8)<0);
    CHECK(f.observe(true,true,2001,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
}
void freshness_and_identity() {
    Fixture f;CHECK(!f.observe());const auto initial=f.read();
    crml_media_state out{};
    CHECK(f.service.read_at(out,f.now+250)==1);
    CHECK(f.service.read_at(out,f.now+251)<0 && out.size==0);
    CHECK(f.service.request_at(7,initial.generation,f.now+251)<0);
    CHECK(f.service.read_at(out,f.now-1)<0 && out.size==0);
    CHECK(f.service.request_at(7,initial.generation,f.now-1)<0);
    CHECK(f.service.request_at(0,initial.generation,f.now)<0);
    CHECK(f.service.request_at(7,0,f.now)<0);
    CHECK(f.service.request_at(7,initial.generation+1,f.now)<0);
    CHECK(f.request()==0);
    ++f.identity;++f.now;
    CHECK(!f.observe(true,true,3000,"media/next.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(f.read().generation>initial.generation);
    CHECK(f.service.request_at(7,initial.generation,f.now)<0);
    CHECK(f.request()==0); // Old target's request cannot reserve the new target.
}
void ownership_expiry_and_once() {
    Fixture f;CHECK(!f.observe());const auto before=f.read();CHECK(f.request()==0);
    f.service.release(8);CHECK(f.request(8)<0);
    CHECK(f.observe(true,true,2500,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(!f.observe(true,true,2600,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(f.service.request_at(7,before.generation,f.now)<0);
    crml_media_state out{};
    if(f.service.read_at(out,f.now)==1) CHECK((out.flags&CRML_MEDIA_SKIPPABLE)==0);
    // An engine still reporting the retired active identity cannot arm it again.
    CHECK(!f.observe(true,true,2700));
    CHECK(f.service.request_at(7,before.generation,f.now)<0);
    CHECK(!f.observe(false));CHECK(!f.observe());
    CHECK(f.read().generation>before.generation);
    CHECK(f.request()==0);f.service.release(7);
    CHECK(!f.observe(true,true,3000,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(f.request(8)==0);f.service.release(8);

    Fixture exact;CHECK(!exact.observe());CHECK(exact.request()==0);
    for(unsigned i=0;i<4;++i) {exact.now+=200;CHECK(!exact.observe());}
    exact.now+=200;
    CHECK(exact.observe(true,true,4000,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    Fixture expired;CHECK(!expired.observe());CHECK(expired.request()==0);
    for(unsigned i=0;i<5;++i) {expired.now+=200;CHECK(!expired.observe());}
    ++expired.now;
    CHECK(!expired.observe(true,true,4000,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(expired.request()==0);
}
void generation_invalidations() {
    Fixture f;CHECK(!f.observe());auto old=f.read().generation;CHECK(f.request()==0);
    CHECK(!f.observe(true,true,2200,"media/replaced.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(f.read().generation>old);old=f.read().generation;CHECK(f.request()==0);
    CHECK(!f.observe(true,true,2200,"media/replaced.bk2",CRML_MEDIA_MAPPED_NAME,true));
    CHECK(f.read().generation>old);old=f.read().generation;CHECK(f.request()==0);
    CHECK(!f.observe(true,true,2100,"media/replaced.bk2",CRML_MEDIA_MAPPED_NAME,true));
    CHECK(f.read().generation>old);old=f.read().generation;CHECK(f.request()==0);
    f.now+=251;
    CHECK(!f.observe(true,true,2200,"media/replaced.bk2",CRML_MEDIA_MAPPED_NAME,true));
    CHECK(f.read().generation>old);
}
void name_provenance() {
    Fixture f;
    for(const auto source:{CRML_MEDIA_ENGINE_NAME,mapped_name}) {
        const std::string name(255,'x');CHECK(!f.observe(true,true,2001,name,source));
        const auto out=f.read();CHECK(out.name_length==255 && out.name[255]=='\0');
        CHECK(std::memcmp(out.name,name.data(),255)==0 && (out.flags&(CRML_MEDIA_ENGINE_NAME|mapped_name))==source);
    }
    const std::string embedded("one\0two",7);
    for(const auto& name:{std::string(),std::string(256,'x'),embedded}) {
        CHECK(!f.observe(true,true,2001,name));const auto out=f.read();
        CHECK(out.name_length==0 && out.name[0]=='\0' && (out.flags&(CRML_MEDIA_ENGINE_NAME|mapped_name))==0);
    }
    for(const auto source:{0u,CRML_MEDIA_ENGINE_NAME|mapped_name,16u}) {
        CHECK(!f.observe(true,true,2001,"media/boot-logo.bk2",source));const auto out=f.read();
        CHECK(out.name_length==0 && out.name[0]=='\0' && (out.flags&(CRML_MEDIA_ENGINE_NAME|mapped_name))==0);
    }
}
void inactive_and_disable() {
    Fixture f;CHECK(!f.observe());const auto original=f.read();CHECK(f.request()==0);
    CHECK(!f.observe(false,true,3000,"",0,true));
    CHECK(f.service.request_at(7,original.generation,f.now)<0);
    CHECK(!f.observe());CHECK(f.read().generation>original.generation);
    CHECK(f.request()==0);f.service.enable(false);f.service.enable(true);
    crml_media_state out{};CHECK(f.service.read_at(out,f.now)<0 && out.size==0);
    CHECK(!f.observe(true,true,3000,"media/boot-logo.bk2",CRML_MEDIA_ENGINE_NAME,true));
    CHECK(f.request()==0);
    Fixture zero;zero.identity=0;CHECK(!zero.observe());
    CHECK(zero.service.request_at(7,1,zero.now)<0);
}
}
int main() {
    try {unavailable_and_guard();freshness_and_identity();ownership_expiry_and_once();generation_invalidations();name_provenance();inactive_and_disable();
        std::cout<<"Media service readiness, freshness, owner cancellation, single consumption and name provenance checks passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
