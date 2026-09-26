#include "physics_session.h"
#include "physics_trial.h"
#include "physics_selection.h"
#include "physics_observation.h"
#include "engine_observer.h"
#include "overlay.h"
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace crml::physics {
namespace {
using namespace crml::observer;
constexpr char game_sha[]="2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0";
constexpr char backend_sha[]="ec53e67b700e67a5420a2c951340efd9a8ed9d086da539eed6ee88368fe0ce51";
using Dispatch=void(*)(void*,uint16_t,void*);
using Move=void(*)(void*,void*,void*,void*,void*,void*);
using Destroy=void(*)(void*);
using Release=void(*)(void*,uint64_t);
Dispatch dispatch_original{}; Move move_original{};
Destroy destroy_original{}; Release release_original{};
uintptr_t image{},backend_image{};
uint64_t salt{};
std::atomic<bool> ready{},accepting{},retired{true};
std::atomic<uint64_t> heartbeat{},request_tick{},watched_owner{},watched_body{},retirement_serial{},retirements{};
std::atomic<unsigned> commands{},retiring{};
std::atomic<int> ui{-1};
std::atomic<uint64_t> dispatches{},rejected_contexts{};
SRWLOCK state_lock=SRWLOCK_INIT;
Buffer events;
DampingTrial trial;
SelectionSearch search;
std::atomic<uint32_t> selection_slots{},selection_scanned{};
std::atomic<uint32_t> selection_candidates{};
std::atomic<float> selection_nearest{-1},selection_second{-1};
Target selected{};
uint64_t serial{},world_token{},owner_token{},actor_token{},selected_tick{};
struct Player { uint64_t world{},entity{},tick{}; float position[3]{}; } player;

uint64_t token(uintptr_t pointer) noexcept { return identity(pointer,salt); }
void record(unsigned action,unsigned result,float before=0,float after=0) noexcept {
    Event e{}; e.kind=Kind::body; e.edge=static_cast<uint8_t>(action); e.flags=result;
    e.qpc=GetTickCount64(); e.thread=GetCurrentThreadId(); e.span=selected.epoch;
    e.object=owner_token; e.entity=selected.entity; e.detail=selected.body;
    e.value=uint64_t(std::bit_cast<uint32_t>(before)) | (uint64_t(std::bit_cast<uint32_t>(after))<<32);
    events.push(e);
}
bool focused() noexcept { DWORD pid{}; GetWindowThreadProcessId(GetForegroundWindow(),&pid); return pid==GetCurrentProcessId(); }
bool down(int key) noexcept { return (GetAsyncKeyState(key)&0x8000)!=0; }
bool position(uintptr_t world,uint64_t entity,float (&out)[3]) noexcept {
    __try {
        uintptr_t chunk{}; uint32_t row{};
        if(probe::entity_component(world,entity,0x9b382c56,32,chunk,row)) return false;
        const auto transform=probe::entity_component(world,entity,0x6cfbb2a9,32,chunk,row);
        if(!transform) return false;
        std::memcpy(out,reinterpret_cast<void*>(transform+16),12);
        for(float v:out) if(!std::isfinite(v)) return false;
        uintptr_t again{}; uint32_t next{};
        return probe::entity_component(world,entity,0x6cfbb2a9,32,again,next)==transform && again==chunk && next==row;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool single_body(const EntityBodySnapshot& candidate) noexcept {
    __try {
        const auto records=*reinterpret_cast<uintptr_t*>(candidate.owner+0xc8);
        const auto instance=*reinterpret_cast<uintptr_t*>(records+uint64_t(candidate.link.scene_slot)*0x70+0x20);
        return candidate.link.local_index==0 && *reinterpret_cast<uint16_t*>(instance+0xa8)==1;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
DampingAccessors accessors() noexcept {
    return {backend_image+0x15dd88,reinterpret_cast<DampingGetter>(backend_image+0x22360),reinterpret_cast<DampingGetter>(backend_image+0x21870)};
}
class Native final:public DampingBackend {
public:
    explicit Native(SimulationContext context):context_(context){}
    Reading read(const Target& target) noexcept override {
        EntityBodySnapshot fresh{};
        const auto status=resolve(target,fresh);
        if(status!=ReadStatus::ok) return {status,0};
        DampingReadback value{};
        if(read_damping_accessors(context_.owner,fresh.body,accessors(),value)!=AccessRead::ok) return {};
        if(retired.load()) return {ReadStatus::retired,0};
        return {ReadStatus::ok,value.linear};
    }
    bool write(const Target& target,float expected,float value) noexcept override {
        EntityBodySnapshot fresh{};
        if(resolve(target,fresh)!=ReadStatus::ok || retired.load() || retiring.load()) return false;
        const auto result=write_linear_damping(context_,fresh,accessors(),reinterpret_cast<DampingSetter>(backend_image+0x24c30),expected,value);
        record(2,unsigned(result.status)|(result.attempted?0x100u:0),expected,value);
        return result.status==WriteStatus::ok && !retired.load();
    }
private:
    ReadStatus resolve(const Target& target,EntityBodySnapshot& out) noexcept {
        if(target!=selected || retired.load()) return ReadStatus::retired;
        if(token(context_.world)!=world_token || token(context_.owner)!=owner_token || retiring.load()) return ReadStatus::unavailable;
        const auto result=read_entity_body(context_.world,target.entity,target.local_index,target.body,accessors().vtable,out);
        if(result!=TargetRead::ok) return ReadStatus::unavailable;
        if(token(out.body.actor)!=actor_token) return ReadStatus::retired;
        return ReadStatus::ok;
    }
    SimulationContext context_;
};
void select(const SimulationContext& context,uint64_t now,bool begin) noexcept {
    if(begin) {selected={};watched_owner=0;retired=true;search.cancel();selection_scanned=0;selection_candidates=0;selection_nearest=-1;selection_second=-1;}
    if(player.world!=token(context.world) || !player.tick || now<player.tick || now-player.tick>500) {search.cancel();ui=3;record(0,1);return;}
    SelectionScope scope{token(context.world),token(context.owner),player.entity,retirement_serial.load(),body_slot_count(context.owner)};
    std::memcpy(scope.position,player.position,sizeof(scope.position));
    selection_slots=scope.slots;
    if(begin && search.begin(scope,now)==SelectionResult::invalid) {ui=3;record(0,2);return;}
    if(retiring.load()) {search.cancel();ui=7;record(0,5);return;}
    const auto result=search.step(scope,now,[&](uint32_t i,SelectionCandidate& candidate) {
        BodySnapshot body{}; BodyEntitySnapshot link{};
        if(read_body(context.owner,i,accessors().vtable,body)!=BodyRead::ok || body.alternate
           || read_body_entity(context.world,context.owner,body,link)!=LinkRead::ok || link.entity==player.entity) return false;
        if(!single_body({context.owner,body,link})) return false;
        float at[3]{}; if(!position(context.world,link.entity,at)) return false;
        float distance{}; for(unsigned a=0;a<3;++a) distance+=(at[a]-search.scope().position[a])*(at[a]-search.scope().position[a]);
        if(!std::isfinite(distance) || distance>4) return false;
        candidate={link.entity,body.handle,token(body.actor),distance};return true;
    },[&] {return GetTickCount64()-now>=2;});
    selection_scanned=search.scanned();
    selection_candidates=search.matches();selection_nearest=search.nearest_distance();selection_second=search.second_distance();
    if(result==SelectionResult::pending) {ui=4;return;}
    if(result!=SelectionResult::selected) {
        ui=result==SelectionResult::none?5:result==SelectionResult::ambiguous?6:result==SelectionResult::timeout?8:7;
        record(0,result==SelectionResult::none?3:result==SelectionResult::ambiguous?4:result==SelectionResult::timeout?6:5);return;
    }
    const auto choice=search.candidate();
    EntityBodySnapshot fresh{};
    float at[3]{};
    if(read_entity_body(context.world,choice.entity,0,choice.body,accessors().vtable,fresh)!=TargetRead::ok
       || token(fresh.body.actor)!=choice.actor || fresh.body.alternate || !single_body(fresh)
       || !position(context.world,choice.entity,at)) {ui=7;record(0,5);return;}
    float distance{};for(unsigned a=0;a<3;++a) distance+=(at[a]-player.position[a])*(at[a]-player.position[a]);
    if(!std::isfinite(distance) || distance>4 || body_slot_count(context.owner)!=scope.slots) {ui=7;record(0,5);return;}
    selected={++serial,fresh.link.entity,fresh.body.handle,0}; selected_tick=now;
    world_token=token(context.world); owner_token=token(context.owner); actor_token=token(fresh.body.actor);
    watched_body.store(selected.body); watched_owner.store(owner_token); retired.store(false);
    // Registration cannot hide a concurrent retirement, including one that
    // completed between resolving the target and publishing these identities.
    if(retiring.load() || retirement_serial.load()!=search.scope().retirement) retired=true;
    if(retired.load()) {selected={};ui=7;record(0,5);return;}
    ui=0;record(0,0,fresh.body.linear_damping,fresh.body.angular_damping);
}
void dispatch(void* view,uint16_t id,void* descriptor) {
    dispatch_original(view,id,descriptor);
    if(!ready.load()) return;
    ++dispatches;
    SimulationContext context{};
    if(!read_simulation_context(reinterpret_cast<uintptr_t>(view),reinterpret_cast<uintptr_t>(descriptor),image+0x190e4d0,context)) {++rejected_contexts;return;}
    if(!TryAcquireSRWLockExclusive(&state_lock)) return;
    const auto now=GetTickCount64(); const auto beat=heartbeat.load();
    const bool keep=accepting.load() && focused() && beat && now>=beat && now-beat<=500 && !down(VK_ESCAPE);
    const auto requested=commands.exchange(0);
    const auto tick=request_tick.load(); const bool recent=tick && now>=tick && now-tick<=500;
    Native native(context);
    if(trial.pending()) {
        const auto result=trial.poll(native,now,keep && !(requested&4));
        if(result!=TrialResult::active) record(1,unsigned(result));
        ui=result==TrialResult::active?1:trial.pending()?3:2;
        if(!trial.pending()) {selected={};watched_owner=0;retired=true;}
    } else if(!keep || (requested&4)) {search.cancel();selected={};watched_owner=0;retired=true;ui=-1;}
    else if(recent && (requested&1)) select(context,now,true);
    else if(search.pending()) select(context,now,false);
    else if(recent && (requested&2) && selected.epoch) {
        if(now<selected_tick || now-selected_tick>15000) {selected={};ui=3;}
        else {
            const auto result=trial.apply(native,selected,8.f,now,5000);
            record(1,unsigned(result)); ui=result==TrialResult::applied?1:3;
            if(!trial.pending()) {selected={};watched_owner=0;retired=true;}
        }
    }
    ReleaseSRWLockExclusive(&state_lock);
}
void movement(void* view,void* world,void* collision,void* callback,void* scene,void* time) {
    move_original(view,world,collision,callback,scene,time);
    if(!ready.load()) return;
    probe::Sample sample{}; bool valid=false;
    __try { valid=probe::inspect(view,world,*reinterpret_cast<uint16_t*>(image+0x5c00ca4),sample)==probe::Observation::player; }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { valid=false; }
    if(valid && TryAcquireSRWLockExclusive(&state_lock)) {
        player.world=token(sample.world); player.entity=sample.entity; player.tick=GetTickCount64();
        std::memcpy(player.position,sample.position,12); ReleaseSRWLockExclusive(&state_lock);
    }
}
void destroy(void* owner) {
    ++retiring; ++retirement_serial;
    if(token(reinterpret_cast<uintptr_t>(owner))==watched_owner.load()) {retired=true;++retirements;}
    destroy_original(owner);
    ++retirement_serial; --retiring;
}
void release(void* owner,uint64_t body) {
    ++retiring; ++retirement_serial;
    if(token(reinterpret_cast<uintptr_t>(owner))==watched_owner.load() && body==watched_body.load()) {retired=true;++retirements;}
    release_original(owner,body);
    ++retirement_serial; --retiring;
}
struct Hook {uint32_t rva; std::array<unsigned char,18> bytes; void* detour; void** original;};
const Hook hooks[]{
    {0x190e4d0,{0x49,0x8b,0x40,0x58,0x4c,0x8b,0x40,0x10,0x48,0x8b,0x50,0x08,0x48,0x8b,0x08,0xe9,0xbc,0x3a},reinterpret_cast<void*>(&dispatch),reinterpret_cast<void**>(&dispatch_original)},
    {0x2ce2d00,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9},reinterpret_cast<void*>(&destroy),reinterpret_cast<void**>(&destroy_original)},
    {0x2d83e20,{0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0xd0,0x01,0,0,0x48},reinterpret_cast<void*>(&release),reinterpret_cast<void**>(&release_original)},
    {0x1b98950,{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x53,0x56,0x57},reinterpret_cast<void*>(&movement),reinterpret_cast<void**>(&move_original)}
};
}
std::string Session::start(const std::filesystem::path& root) {
    wchar_t path[32768]{}; const auto size=GetModuleFileNameW(nullptr,path,32768);
    if(!size || size>=32768 || module_fingerprint(path)!=game_sha) return "Physics trial refused: unsupported executable fingerprint";
    auto backend=GetModuleHandleW(L"PhysX_64.dll");
    const auto length=backend?GetModuleFileNameW(backend,path,32768):0;
    if(!length || length>=32768 || module_fingerprint(path)!=backend_sha) return "Physics trial refused: unsupported backend fingerprint";
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); backend_image=reinterpret_cast<uintptr_t>(backend);
    for(const auto& hook:hooks) if(std::memcmp(reinterpret_cast<void*>(image+hook.rva),hook.bytes.data(),hook.bytes.size())) return "Physics trial refused: changed hook entry";
    const auto init=MH_Initialize(); if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return "Physics trial refused: hook initialization";
    size_t created{};
    for(const auto& hook:hooks) {
        if(MH_CreateHook(reinterpret_cast<void*>(image+hook.rva),hook.detour,hook.original)!=MH_OK) break;
        ++created;
    }
    if(created!=std::size(hooks)) {
        for(size_t i=0;i<created;++i) MH_RemoveHook(reinterpret_cast<void*>(image+hooks[i].rva));
        return "Physics trial refused: hook creation";
    }
    started_=GetTickCount64(); LARGE_INTEGER clock{}; QueryPerformanceCounter(&clock); salt=clock.QuadPart;
    auto name="physics-trial-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(started_)+".jsonl";
    log_.open(root/name,std::ios::out|std::ios::binary);
    if(!log_) return "Physics trial refused: cannot open log";
    std::ostringstream header;
    header<<"{\"type\":\"header\",\"schema\":1,\"mode\":\"native-physics-trial\",\"sha256\":\""<<game_sha<<"\",\"physx_sha256\":\""<<backend_sha<<"\",\"mods_suspended\":true}\n";
    const auto header_text=header.str();log_<<header_text;bytes_=header_text.size();
    log_.flush(); if(!log_) return "Physics trial refused: cannot write log";
    overlay_=probe::overlay_create(true);
    if(!overlay_) return "Physics trial refused: overlay unavailable";
    events.open();
    for(const auto& hook:hooks) if(MH_EnableHook(reinterpret_cast<void*>(image+hook.rva))!=MH_OK) return "Physics trial refused: hook enable";
    accepting=true; ready=true; active_=true;
    return "Native physics trial ready: F9 select, F10 apply for five seconds, F11/Esc restore; log: "+name;
}
void Session::poll() {
    if(!active_) return;
    const auto now=GetTickCount64(); heartbeat=now;
    const bool focus=focused();
    const unsigned current=focus?(unsigned(down(VK_F9))|unsigned(down(VK_F10))*2|unsigned(down(VK_F11)||down(VK_ESCAPE))*4):0;
    const auto edges=current&~keys_; keys_=current;
    if(now-started_>=600000 || !log_.is_open() || !log_) accepting=false;
    if(!accepting.load() || !focus) commands.fetch_or(4);
    else if(edges && probe::overlay_diagnostics().frames) {request_tick=now;commands.fetch_or(edges);}
    probe::overlay_update(overlay_,focus,ui.load());
    if(!log_.is_open() || !log_) return;
    std::array<Event,128> batch{};
    const auto count=events.drain(batch.data(),batch.size());
    for(size_t i=0;i<count;++i) {
        const auto& e=batch[i]; std::ostringstream text; text<<std::setprecision(9);
        text<<"{\"type\":\"event\",\"sequence\":"<<e.sequence<<",\"tick_ms\":"<<e.qpc<<",\"thread\":"<<e.thread
            <<",\"action\":"<<unsigned(e.edge)<<",\"result\":"<<e.flags<<",\"selection\":"<<e.span
            <<",\"scene\":\""<<e.object<<"\",\"entity\":\""<<e.entity<<"\",\"body\":\""<<e.detail
            <<"\",\"before\":"<<std::bit_cast<float>(uint32_t(e.value))<<",\"after\":"<<std::bit_cast<float>(uint32_t(e.value>>32))<<"}\n";
        auto line=text.str();
        if(bytes_+line.size()>4*1024*1024-2048) {accepting=false;commands.fetch_or(4);log_<<"{\"type\":\"end\",\"reason\":\"size_limit\"}\n";log_.close();break;}
        log_<<line;bytes_+=line.size();
    }
    if(log_.is_open() && log_ && now-last_stats_>=1000) {
        std::ostringstream text;
        text<<"{\"type\":\"stats\",\"tick_ms\":"<<now<<",\"dispatches\":"<<dispatches.load()<<",\"context_rejections\":"<<rejected_contexts.load()
            <<",\"retirements\":"<<retirements.load()<<",\"dropped\":"<<events.dropped()
            <<",\"selection_slots\":"<<selection_slots.load()<<",\"selection_scanned\":"<<selection_scanned.load()
            <<",\"selection_candidates\":"<<selection_candidates.load()<<",\"nearest_distance\":"<<selection_nearest.load()
            <<",\"second_distance\":"<<selection_second.load()
            <<",\"overlay\":\""<<probe::overlay_diagnostics().status<<"\"}\n";
        const auto line=text.str();
        if(bytes_+line.size()>4*1024*1024-2048) {accepting=false;commands.fetch_or(4);log_<<"{\"type\":\"end\",\"reason\":\"size_limit\"}\n";log_.close();}
        else {log_<<line;bytes_+=line.size();last_stats_=now;}
    }
    if(log_.is_open() && log_) log_.flush();
}
Session::~Session() { accepting=false; commands.fetch_or(4); if(overlay_) probe::overlay_destroy(overlay_); }
#ifdef CRML_PHYSICS_SESSION_TESTING
bool test_session_prologues() {
    const auto init=MH_Initialize(); if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    bool ok=true;
    for(const auto& hook:hooks) {
        auto memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        if(!memory) return false;
        std::memcpy(memory,hook.bytes.data(),hook.bytes.size());void* trampoline{};
        const auto result=MH_CreateHook(memory,hook.detour,&trampoline);
        ok=ok && result==MH_OK && trampoline;
        if(result==MH_OK) MH_RemoveHook(memory);VirtualFree(memory,0,MEM_RELEASE);
    }
    // Exercise the real hook-side invalidation before synthetic destruction.
    const auto old_destroy=destroy_original; const auto old_release=release_original;
    destroy_original=+[](void*){}; release_original=+[](void*,uint64_t){};
    watched_owner=token(0x1000);watched_body=0x500000003ull;retired=false;
    release(reinterpret_cast<void*>(0x2000),watched_body.load());ok=ok && !retired.load();
    release(reinterpret_cast<void*>(0x1000),0x700000003ull);ok=ok && !retired.load();
    release(reinterpret_cast<void*>(0x1000),watched_body.load());ok=ok && retired.load();
    retired=false;destroy(reinterpret_cast<void*>(0x2000));ok=ok && !retired.load();
    destroy(reinterpret_cast<void*>(0x1000));ok=ok && retired.load() && !retiring.load();
    watched_owner=0;destroy_original=old_destroy;release_original=old_release;
    return ok;
}
#endif
}
