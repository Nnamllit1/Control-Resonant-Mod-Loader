#include "tutorial_native.h"
#include "tutorial_payload.h"
#include "mod_tutorials.h"
#include "compatibility.h"
#include "movement_view.h"
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <array>
#include <cstring>
#include <ostream>

namespace crml::tutorial_native {
namespace {
using namespace tutorial;
using Dispatch=void(*)(const void*,uint16_t,const void*);
using Events=void(*)(const void*,void*,void*,void*);
using Sync=void(*)(const void*,void*,const void*,const void*,void*,void*,void*);
struct Value {PageVector pages{};uint32_t layout{},padding{};};
struct InsertResult {void* control{};void* slot{};bool inserted{};uint8_t padding[7]{};};
using Insert=InsertResult*(*)(void*,InsertResult*,const uint32_t*,uint64_t,uintptr_t,const uint32_t**,Value**);
using Cleanup=void(*)(void*);
Dispatch original_dispatch{};Events original_events{};Sync original_sync{};
Insert insert{};Cleanup cleanup{};
PanelNative panel{};PayloadNative payload{};
uintptr_t image{};
std::atomic<bool> enabled{},faulted{};
std::atomic<uint64_t> presentations{},retirements{};
std::atomic<int> cancellation_result{-1};
bool installed{};
SRWLOCK gate=SRWLOCK_INIT;
thread_local bool entered{};
// Exactly one private native table may be live. Faults quarantine it and close
// admission for this process, so retries cannot accumulate leaked allocations.
struct Live {
    uint64_t ticket{};
    PanelOwner owner{};
    uintptr_t data{};
    alignas(16) std::array<unsigned char,0x70> shadow{};
    bool opened{},presented{},cancel{},sync_retired{};
} live;
uint32_t next_key=0xe7a10000;
template<class T> T read(uintptr_t p) noexcept {T v;std::memcpy(&v,reinterpret_cast<void*>(p),sizeof(v));return v;}
bool memory(uintptr_t p,size_t n) noexcept {
    if(!p || !n || p>UINTPTR_MAX-n) return false;
    const auto end=p+n;
    while(p<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto protection=m.Protect&0xff;
        if(protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY && protection!=PAGE_EXECUTE_READ &&
           protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY) return false;
        const auto next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        if(next<=p) return false;p=next;
    }
    return true;
}
void fail() noexcept {faulted.store(true);enabled.store(false);process_tutorials().enable(CRML_TUTORIAL_PANEL,false);}
uint64_t hash(uint32_t key) noexcept {uint64_t high{};const auto low=_umul128(key,0xde5fb9d2630458e9ULL,&high);return low+high;}
bool construct(Value& value) noexcept {
    const uint32_t* key=&live.owner.key;Value* input=&value;InsertResult result{};
    __try {
        insert(live.shadow.data()+0x38,&result,key,hash(*key),0,&key,&input);
        return result.inserted && result.slot && !value.pages.data && !value.pages.count && !value.pages.capacity;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool destroy() noexcept {
    __try {cleanup(live.shadow.data()+0x38);return !read<uintptr_t>(reinterpret_cast<uintptr_t>(live.shadow.data()+0x38));}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool idle(void* state) noexcept {
    __try {const auto s=reinterpret_cast<uintptr_t>(state);return memory(s,0x40) && !read<uint8_t>(s+8) && !read<uint8_t>(s+0x14) && !read<uint8_t>(s+0x30);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool owned(void* state) noexcept {
    __try {const auto s=reinterpret_cast<uintptr_t>(state);return live.ticket && s==live.owner.state && memory(s,0x40) &&
        read<uint32_t>(s+0xc)==live.owner.key && read<uint8_t>(s+0x10)==1;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool choose_key(uintptr_t data,uint32_t& key) noexcept {
    __try {
        if(!memory(data,0x70)) return false;
        for(unsigned attempt=0;attempt<64;++attempt) {
            const auto candidate=++next_key;
            bool collision=false;
            for(unsigned secondary=0;secondary<2;++secondary) {
                const auto table=data+(secondary?0x38:8);
                const auto controls=read<uintptr_t>(table),slots=read<uintptr_t>(table+8),mask=read<uint64_t>(table+0x18);
                if(!controls && !mask) continue;
                const size_t stride=secondary?0x20:0x68;
                if(!mask || mask>65535 || ((mask+1)&mask) || !memory(controls,static_cast<size_t>(mask)) ||
                   !memory(slots,static_cast<size_t>(mask)*stride)) return false;
                for(size_t i=0;i<mask;++i)
                    if(read<uint8_t>(controls+i)<0x80 && read<uint32_t>(slots+i*stride)==candidate) {collision=true;break;}
                if(collision) break;
            }
            if(!collision) {key=candidate;return true;}
        }
        return false;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
// Revalidate the actual ECS component/full generation on every sync. The
// retained addresses only identify the lease; they never admit a new world.
bool sync_identity(const void* query,void* state) noexcept {
    __try {
        const auto q=reinterpret_cast<uintptr_t>(query);
        if(!live.ticket || reinterpret_cast<uintptr_t>(state)!=live.owner.state || !memory(q,24)) return false;
        const auto base=read<uintptr_t>(q),chunk=read<uintptr_t>(q+8),row=read<uintptr_t>(q+16);
        if(row>=16384 || !memory(chunk,0x10+(row+1)*8) || read<uint64_t>(chunk+0x10+row*8)!=live.owner.identity.entity) return false;
        uintptr_t actual_chunk{};uint32_t actual_row{};
        const auto actual=probe::entity_component(live.owner.identity.world,live.owner.identity.entity,0xae510289,0x70,actual_chunk,actual_row);
        return actual && actual==base+row*0x70 && actual==live.data && actual_chunk==chunk && actual_row==row;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
}
bool substitute_events(const PanelCall& call,std::array<uintptr_t,4>& query) noexcept {
    __try {
        if(!live.ticket || !live.opened) return false;
        const auto s=reinterpret_cast<uintptr_t>(call.state),q=reinterpret_cast<uintptr_t>(call.query);
        const bool pending=read<uint8_t>(s+8)!=0;
        if(pending && read<uint8_t>(s+4)<=1 && (read<uint8_t>(s+4)!=1 || read<uint32_t>(s)!=live.owner.key)) return false;
        if(!owned(call.state) && !(pending && read<uint8_t>(s+4)==1 && read<uint32_t>(s)==live.owner.key)) return false;
        const auto row=read<uintptr_t>(q+24);
        query={reinterpret_cast<uintptr_t>(live.shadow.data()),read<uintptr_t>(q+8)+row*16,read<uintptr_t>(q+16),0};
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {fail();return false;}
}
void finish() {
    if(!destroy()) {fail();return;}
    const auto ticket=live.ticket;const auto cancelled=live.cancel;
    live={};retirements.fetch_add(1);
    process_tutorials().report(ticket,cancelled?CRML_TUTORIAL_CANCELLED:CRML_TUTORIAL_DISMISSED);
}
void before_events(const PanelCall& call) {
    if(faulted.load()) return;
    Identity identity{};
    if(PanelScope::identify(call,identity)!=IdentityStatus::ok) return;
    if(live.ticket) {
        if(identity.world!=live.owner.identity.world || identity.entity!=live.owner.identity.entity ||
           reinterpret_cast<uintptr_t>(call.state)!=live.owner.state) return;
        const auto* words=static_cast<const uintptr_t*>(call.query);
        live.data=words[0]+words[3]*0x70;
        // Event and sync acknowledgements belong to the same current identity.
        live.cancel=live.cancel || !enabled.load() || process_tutorials().cancelled(live.ticket);
        if(live.sync_retired && idle(call.state) && PanelScope::release_owned(call,live.owner)) {finish();return;}
        if(live.cancel && owned(call.state)) {
            const auto result=PanelScope::close_owned(call,live.owner);
            cancellation_result.store(static_cast<int>(result));
            if(result==PanelClose::partial) fail();
        }
        return;
    }
    if(!enabled.load() || !idle(call.state)) return;
    const auto* words=static_cast<const uintptr_t*>(call.query);
    const auto data=words[0]+words[3]*0x70;
    ModTutorials::Request request;
    if(!process_tutorials().take(CRML_TUTORIAL_PANEL,request)) return;
    uint32_t key{};
    if(!choose_key(data,key)) {process_tutorials().report(request.ticket,CRML_TUTORIAL_FAILED);return;}
    live.ticket=request.ticket;
    cancellation_result.store(-1);
    live.owner={identity,reinterpret_cast<uintptr_t>(call.state),key,1};
    live.data=data;
    Value value{};
    // The native font renders Unicode format separators as missing glyphs.
    // Keep authored text unchanged; ordinary missing localization keys echo it.
    const std::string body=tutorial_body_markup(request.body,request.image_url,request.image);
    if(!payload.make(request.title,body,value.pages) || !construct(value)) {fail();return;}
    if(process_tutorials().cancelled(live.ticket)) {
        live.cancel=true;finish();return;
    }
    live.opened=PanelScope::open_owned(call,live.owner);
    if(!live.opened) {
        // Nothing was published, so this private table has no native readers.
        const auto ticket=live.ticket;
        if(!destroy()) {fail();return;}live={};process_tutorials().report(ticket,CRML_TUTORIAL_FAILED);
    }
}
void events_body(const PanelCall& call) {
    try {before_events(call);} catch(...) {fail();}
    std::array<uintptr_t,4> query{};
    Identity identity{};
    const bool matches=live.ticket && PanelScope::identify(call,identity)==IdentityStatus::ok &&
        identity.world==live.owner.identity.world && identity.entity==live.owner.identity.entity &&
        reinterpret_cast<uintptr_t>(call.state)==live.owner.state;
    const bool substitute=matches && substitute_events(call,query);
    original_events(substitute?query.data():call.query,call.state,call.stacks,call.audio);
}
void dispatch_hook(const void* view,uint16_t id,const void* system) {
    PanelScope::invoke(view,id,system,panel,original_dispatch);
}
void events_hook(const void* query,void* state,void* stacks,void* audio) {
    const PanelCall call{query,state,stacks,audio,reinterpret_cast<uintptr_t>(_ReturnAddress())};
    if(entered) {original_events(query,state,stacks,audio);return;}
    AcquireSRWLockExclusive(&gate);entered=true;
    __try {events_body(call);}
    __finally {entered=false;ReleaseSRWLockExclusive(&gate);}
}
void observe_sync(void* state,bool substitute) {
    if(faulted.load()) return;
    __try {
        const auto s=reinterpret_cast<uintptr_t>(state);
        if(!memory(s,0x40)) {fail();return;}
        if(substitute && read<uint8_t>(s+0x14) && read<uint8_t>(s+0x30) && !live.presented) {
            live.presented=true;presentations.fetch_add(1);process_tutorials().report(live.ticket,CRML_TUTORIAL_PRESENTED);
        }
        // The native sync returned through its input-restoration path. Only
        // then can the next verified event boundary retire the private record.
        if(live.opened && !read<uint8_t>(s+0x14) && !read<uint8_t>(s+0x30) && !read<uint8_t>(s+8)) live.sync_retired=true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {fail();}
}
void sync_body(const void* query,void* state,const void* c,const void* d,void* e,void* f,void* g,uintptr_t caller) {
    const bool match=caller==image+0x1e62743 && sync_identity(query,state);
    const bool substitute=match && live.opened && owned(state);
    const std::array<uintptr_t,3> shadow{reinterpret_cast<uintptr_t>(live.shadow.data()),0,0};
    original_sync(substitute?shadow.data():query,state,c,d,e,f,g);
    if(match) observe_sync(state,substitute);
}
void sync_hook(const void* a,void* b,const void* c,const void* d,void* e,void* f,void* g) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(entered) {original_sync(a,b,c,d,e,f,g);return;}
    AcquireSRWLockExclusive(&gate);entered=true;
    __try {sync_body(a,b,c,d,e,f,g,caller);}
    __finally {entered=false;ReleaseSRWLockExclusive(&gate);}
}
}
bool start() noexcept {
    if(installed) return enabled.load();
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!PanelNative::bind(image,panel) || !PayloadNative::bind(image,payload)) return false;
    struct Site {uintptr_t rva;std::array<unsigned char,32> bytes;};
    constexpr Site sites[]{
        {0x20425c0,{72,137,92,36,16,72,137,108,36,24,72,137,116,36,32,87,65,84,65,85,65,86,65,87,72,131,236,32,72,139,41,72}},
        {0x1e63800,{72,137,92,36,8,72,137,108,36,16,72,137,116,36,24,72,137,124,36,32,65,86,72,131,236,32,72,139,217,72,139,105}},
        {0x1e60d50,{72,137,92,36,8,72,137,116,36,16,72,137,124,36,24,76,137,116,36,32,85,72,141,108,36,169,72,129,236,224,0,0}},
        {0x1e61470,{76,137,76,36,32,76,137,68,36,24,85,83,86,87,65,85,65,87,72,141,108,36,233,72,129,236,200,0,0,0,197,251}}
    };
    for(const auto& site:sites) if(!compatibility::matches(reinterpret_cast<void*>(image+site.rva),site.bytes.data(),site.bytes.size())) return false;
    insert=reinterpret_cast<Insert>(image+0x20425c0);cleanup=reinterpret_cast<Cleanup>(image+0x1e63800);
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    struct Hook {uintptr_t rva;void* detour;void** original;};
    const Hook hooks[]{
        {panel_dispatch_rva,reinterpret_cast<void*>(&dispatch_hook),reinterpret_cast<void**>(&original_dispatch)},
        {0x1e60d50,reinterpret_cast<void*>(&events_hook),reinterpret_cast<void**>(&original_events)},
        {0x1e61470,reinterpret_cast<void*>(&sync_hook),reinterpret_cast<void**>(&original_sync)}
    };
    size_t created=0,activated=0;
    for(const auto& hook:hooks) {
        if(MH_CreateHook(reinterpret_cast<void*>(image+hook.rva),hook.detour,hook.original)!=MH_OK) break;
        ++created;
    }
    if(created==std::size(hooks)) for(const auto& hook:hooks) {
        if(MH_EnableHook(reinterpret_cast<void*>(image+hook.rva))!=MH_OK) break;
        ++activated;
    }
    if(activated!=std::size(hooks)) {
        for(size_t i=0;i<activated;++i) MH_DisableHook(reinterpret_cast<void*>(image+hooks[i].rva));
        for(size_t i=0;i<created;++i) MH_RemoveHook(reinterpret_cast<void*>(image+hooks[i].rva));
        return false;
    }
    installed=true;enabled.store(true);process_tutorials().enable(CRML_TUTORIAL_PANEL,true);return true;
}
void stop() noexcept {enabled.store(false);process_tutorials().enable(CRML_TUTORIAL_PANEL,false);}
void poll(std::ostream& log) {
    static uint64_t previous_shown=UINT64_MAX,previous_retired=UINT64_MAX;static bool previous_fault{};static int previous_cancel=-1;
    const auto shown=presentations.load(),retired=retirements.load();const auto failed=faulted.load();
    const auto cancel=cancellation_result.load();
    if(shown==previous_shown && retired==previous_retired && failed==previous_fault && cancel==previous_cancel) return;
    previous_shown=shown;previous_retired=retired;previous_fault=failed;
    previous_cancel=cancel;
    log<<"Native tutorial panels: enabled="<<enabled.load()<<" presented="<<shown<<" retired="<<retired<<" quarantined="<<failed<<" cancel_result="<<cancel<<'\n';
}
#ifdef CRML_TUTORIAL_NATIVE_TESTING
bool test_contract() {
    static bool abi{},freed{};
    std::array<unsigned char,0x70> stock{};
    std::array<unsigned char,0x68> primary{};std::array<unsigned char,0x20> secondary{};
    const std::array<unsigned char,2> control{0,0xff};
    const auto put=[](unsigned char* target,const auto& value) {std::memcpy(target,&value,sizeof(value));};
    const uint32_t first=next_key+1,second=next_key+2;
    put(primary.data(),first);put(secondary.data(),second);
    for(unsigned i=0;i<2;++i) {
        auto* table=stock.data()+(i?0x38:8);
        put(table,reinterpret_cast<uintptr_t>(control.data()));
        put(table+8,reinterpret_cast<uintptr_t>(i?secondary.data():primary.data()));
        put(table+0x18,uint64_t{1});
    }
    uint32_t chosen{};
    if(!choose_key(reinterpret_cast<uintptr_t>(stock.data()),chosen) || chosen!=second+1) return false;
    put(stock.data()+8+0x18,uint64_t{12});
    if(choose_key(reinterpret_cast<uintptr_t>(stock.data()),chosen)) return false;
    insert=+[](void* table,InsertResult* result,const uint32_t* key,uint64_t mixed,uintptr_t unused,const uint32_t** key_pointer,Value** value_pointer)->InsertResult* {
        abi=table==live.shadow.data()+0x38 && key==&live.owner.key && *key_pointer==key &&
            mixed==hash(*key) && unused==0 && (*value_pointer)->pages.count==1 && !(*value_pointer)->layout;
        auto* slot=live.shadow.data()+0x08;
        std::memcpy(slot,key,4);std::memcpy(slot+8,&(*value_pointer)->pages,sizeof(PageVector));
        (*value_pointer)->pages={};*result={table,slot,true,{}};return result;
    };
    cleanup=+[](void* table) {freed=table==live.shadow.data()+0x38;std::memset(table,0,0x30);};
    auto& service=process_tutorials();constexpr uint64_t owner=0x1234;
    service.enable(CRML_TUTORIAL_PANEL,true);if(!service.attach(owner)) return false;
    const auto ticket=service.show(owner,CRML_TUTORIAL_PANEL,"Title","Body",0);
    ModTutorials::Request request;
    if(ticket<=0 || !service.take(CRML_TUTORIAL_PANEL,request)) return false;
    std::array<unsigned char,0x40> state{};
    live={};live.ticket=request.ticket;live.owner.key=0xe7a10001;
    live.owner.state=reinterpret_cast<uintptr_t>(state.data());live.owner.mode=1;live.opened=true;
    Value value{{reinterpret_cast<void*>(0x12340000),1,1},0,0};
    if(!construct(value) || !abi || value.pages.data) return false;
    std::memcpy(state.data()+0xc,&live.owner.key,4);state[0x10]=1;state[0x14]=state[0x30]=1;
    const std::array<uintptr_t,4> original{0x10000,0x20000,0x30000,7};
    const PanelCall call{original.data(),state.data(),nullptr,nullptr,0};
    std::array<uintptr_t,4> substituted{};
    if(!substitute_events(call,substituted) || substituted[0]!=reinterpret_cast<uintptr_t>(live.shadow.data()) ||
       substituted[1]!=0x20070 || substituted[2]!=original[2] || substituted[3]!=0 || original[3]!=7) return false;
    // Foreign open variants must keep the complete original native query.
    state[8]=1;state[4]=0;
    if(substitute_events(call,substituted)) return false;
    state[4]=1;uint32_t foreign=99;std::memcpy(state.data(),&foreign,4);
    if(substitute_events(call,substituted)) return false;
    std::memcpy(state.data(),&live.owner.key,4);
    if(!substitute_events(call,substituted)) return false;
    state[8]=0;observe_sync(state.data(),true);
    if(!live.presented || service.status(owner,ticket)!=CRML_TUTORIAL_PRESENTED) return false;
    service.dismiss(owner,ticket);live.cancel=true;state[0x14]=0;observe_sync(state.data(),true);
    if(live.sync_retired || freed) return false;
    state[0x30]=0;state[8]=1;observe_sync(state.data(),true);
    if(live.sync_retired || freed) return false;
    state[8]=0;observe_sync(state.data(),true);
    if(!live.sync_retired || freed) return false;
    finish();
    const bool ok=freed && !live.ticket && service.status(owner,ticket)==CRML_TUTORIAL_CANCELLED;
    service.detach(owner);service.enable(CRML_TUTORIAL_PANEL,false);return ok;
}
#endif
}
