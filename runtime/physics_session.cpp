#include "compatibility.h"
#include "input_filter.h"
#include "physics_session.h"
#include "physics_trial.h"
#include "physics_selection.h"
#include "physics_target_diagnostics.h"
#include "physics_lifetime_diagnostics.h"
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
TargetDiagnostics target_diagnostics;
LifetimeDiagnostics lifetime_diagnostics;
uint64_t search_serial{};
unsigned diagnosed_candidates{};
std::array<uint64_t,16> diagnosed_entities{};
std::atomic<uint32_t> selection_slots{},selection_scanned{};
std::atomic<uint32_t> selection_candidates{};
std::atomic<float> selection_nearest{-1},selection_second{-1};
Target selected{};
uint64_t serial{},world_token{},owner_token{},actor_token{},selected_tick{};
uint64_t next_motion{},after_until{};
std::atomic<uint64_t> guest_owner{};
std::atomic<bool> guest_releasing{};
bool guest_mode{}; // Set before hooks are enabled.
int guest_state{};
float requested_damping=8.f;
uint32_t requested_duration=5000;
uint64_t requested_target{};
struct StateGuard {
    bool held=TryAcquireSRWLockExclusive(&state_lock)!=FALSE;
    ~StateGuard() {if(held) ReleaseSRWLockExclusive(&state_lock);}
};
struct Player { uint64_t world{},entity{},tick{}; float position[3]{}; } player;

uint64_t token(uintptr_t pointer) noexcept { return identity(pointer,salt); }
void record(unsigned action,unsigned result,float before=0,float after=0) noexcept {
    Event e{}; e.kind=Kind::body; e.edge=static_cast<uint8_t>(action); e.flags=result;
    e.qpc=GetTickCount64(); e.thread=GetCurrentThreadId(); e.span=selected.epoch;
    e.object=owner_token; e.entity=selected.entity; e.detail=selected.body;
    e.value=uint64_t(std::bit_cast<uint32_t>(before)) | (uint64_t(std::bit_cast<uint32_t>(after))<<32);
    events.push(e);
}
#ifdef CRML_PHYSICS_SESSION_TESTING
int test_focus=-1;
#endif
bool focused() noexcept {
#ifdef CRML_PHYSICS_SESSION_TESTING
    if(test_focus>=0) return test_focus!=0;
#endif
    DWORD pid{}; GetWindowThreadProcessId(GetForegroundWindow(),&pid); return pid==GetCurrentProcessId();
}
bool down(int key) noexcept {
#ifdef CRML_PHYSICS_SESSION_TESTING
    if(test_focus>=0) return false;
#endif
    return probe::input::down(static_cast<unsigned>(key));
}
bool position(uintptr_t world,uint64_t entity,float (&out)[3],bool exclude_controller=true) noexcept {
    __try {
        uintptr_t chunk{}; uint32_t row{};
        if(exclude_controller && probe::entity_component(world,entity,0x9b382c56,32,chunk,row)) return false;
        const auto transform=probe::entity_component(world,entity,0x6cfbb2a9,32,chunk,row);
        if(!transform) return false;
        std::memcpy(out,reinterpret_cast<void*>(transform+16),12);
        for(float v:out) if(!std::isfinite(v)) return false;
        uintptr_t again{}; uint32_t next{};
        return probe::entity_component(world,entity,0x6cfbb2a9,32,again,next)==transform && again==chunk && next==row;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
unsigned local_body_count(const EntityBodySnapshot& candidate) noexcept {
    __try {
        const auto records=*reinterpret_cast<uintptr_t*>(candidate.owner+0xc8);
        const auto instance=*reinterpret_cast<uintptr_t*>(records+uint64_t(candidate.link.scene_slot)*0x70+0x20);
        return *reinterpret_cast<uint16_t*>(instance+0xa8);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool single_body(const EntityBodySnapshot& candidate) noexcept {
    return candidate.link.local_index==0 && local_body_count(candidate)==1;
}
void diagnose(const SimulationContext& context,uint64_t entity,uint64_t body,unsigned count,unsigned exclusion,
              const float (&at)[3],float distance,bool is_player=false) noexcept {
    if(!is_player) {
        for(unsigned i=0;i<diagnosed_candidates;++i) if(diagnosed_entities[i]==entity) return;
        if(diagnosed_candidates>=diagnosed_entities.size()) return;
        diagnosed_entities[diagnosed_candidates++]=entity;
    }
    TargetDiagnostic d{};d.search=search_serial;d.tick=GetTickCount64();d.thread=GetCurrentThreadId();
    d.scene=token(context.owner);d.player=player.entity;d.entity=entity;d.body=body;
    d.body_count=count;d.exclusion=exclusion;d.distance=distance;d.player_sample=is_player;
    std::memcpy(d.player_position,player.position,sizeof(d.player_position));std::memcpy(d.position,at,sizeof(d.position));
    uintptr_t chunk{};uint32_t row{};
    if(probe::entity_component(context.world,entity,0x6cfbb2a9,32,chunk,row)) {
        probe::Sample sample{};sample.world=context.world;sample.entity=entity;sample.row=row;
        d.components_valid=probe::inspect_entity(sample,d.snapshot);
    }
    target_diagnostics.push(d);
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
        // Recheck player/controller/attachment exclusion on access, not just when
        // selecting: an entity can acquire components after the search.
        if(entity_exclusion(context_.world,target.entity,player.entity)) return ReadStatus::unavailable;
        const auto result=read_entity_body(context_.world,target.entity,target.local_index,target.body,accessors().vtable,out);
        if(result!=TargetRead::ok) return ReadStatus::unavailable;
        if(token(out.body.actor)!=actor_token) return ReadStatus::retired;
        return ReadStatus::ok;
    }
    SimulationContext context_;
};
void select(const SimulationContext& context,uint64_t now,bool begin) noexcept {
    if(begin) {selected={};watched_owner=0;retired=true;after_until=next_motion=0;search.cancel();selection_scanned=0;selection_candidates=0;selection_nearest=-1;selection_second=-1;++search_serial;diagnosed_candidates=0;}
    if(player.world!=token(context.world) || !player.tick || now<player.tick || now-player.tick>500) {search.cancel();ui=3;record(0,1);return;}
    SelectionScope scope{token(context.world),token(context.owner),player.entity,retirement_serial.load(),body_slot_count(context.owner)};
    std::memcpy(scope.position,player.position,sizeof(scope.position));
    selection_slots=scope.slots;
    if(begin && search.begin(scope,now)==SelectionResult::invalid) {ui=3;record(0,2);return;}
    if(begin) diagnose(context,player.entity,0,0,1,player.position,0,true);
    if(retiring.load()) {search.cancel();ui=7;record(0,5);return;}
    const auto result=search.step(scope,now,[&](uint32_t i,SelectionCandidate& candidate) {
        BodySnapshot body{}; BodyEntitySnapshot link{};
        if(read_body(context.owner,i,accessors().vtable,body)!=BodyRead::ok
           || read_body_entity(context.world,context.owner,body,link)!=LinkRead::ok) return false;
        float at[3]{}; if(!position(context.world,link.entity,at,false)) return false;
        float distance{}; for(unsigned a=0;a<3;++a) distance+=(at[a]-search.scope().position[a])*(at[a]-search.scope().position[a]);
        if(!std::isfinite(distance) || distance>4) return false;
        const auto count=local_body_count({context.owner,body,link});
        const auto entity_reason=entity_exclusion(context.world,link.entity,player.entity);
        const unsigned exclusion=entity_reason?entity_reason:count!=1 || link.local_index!=0?3:body.alternate?4:0;
        diagnose(context,link.entity,body.handle,count,exclusion,at,std::sqrt(distance));
        if(exclusion) return false;
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
       || entity_exclusion(context.world,choice.entity,player.entity)
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
    lifetime_diagnostics.arm({selected.epoch,owner_token,selected.entity,selected.body});
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
    if(requested && (requested!=4 || trial.pending() || selected.epoch)) record(5,requested);
    const auto tick=request_tick.load(); const bool recent=tick && now>=tick && now-tick<=500;
    Native native(context);
    if(trial.pending()) {
        const auto result=trial.poll(native,now,keep && !(requested&4));
        if(result!=TrialResult::active) record(1,unsigned(result));
        ui=result==TrialResult::active?1:trial.pending()?3:2;
        if(!trial.pending()) after_until=now+5000;
        guest_state=trial.pending()?(result==TrialResult::active?4:5):result==TrialResult::retired?7:result==TrialResult::conflict?8:6;
    } else if(!keep || (requested&4)) {search.cancel();selected={};watched_owner=0;retired=true;after_until=0;ui=-1;}
    else if(recent && (requested&1)) select(context,now,true);
    else if(search.pending()) select(context,now,false);
    else if(recent && (requested&2) && selected.epoch && ui.load()==0 && (!guest_mode || requested_target==selected.epoch)) {
        if(now<selected_tick || now-selected_tick>15000) {selected={};ui=3;}
        else {
            const auto result=trial.apply(native,selected,guest_mode?requested_damping:8.f,now,guest_mode?requested_duration:5000);
            record(1,unsigned(result)); ui=result==TrialResult::applied?1:3;
            guest_state=trial.pending()?(result==TrialResult::applied?4:5):result==TrialResult::retired?7:result==TrialResult::conflict?8:result==TrialResult::unchanged?6:9;
            if(!trial.pending()) after_until=now+5000;
        }
    }
    if(selected.epoch && !retired.load() && now>=next_motion) {
        next_motion=now+100;
        EntityBodySnapshot fresh{};MotionSnapshot motion{};
        auto status=MotionRead::target;
        if(token(context.world)==world_token && token(context.owner)==owner_token && !retiring.load()
           && !entity_exclusion(context.world,selected.entity,player.entity)
           && read_entity_body(context.world,selected.entity,selected.local_index,selected.body,accessors().vtable,fresh)==TargetRead::ok
           && token(fresh.body.actor)==actor_token) status=read_body_motion(context.owner,fresh.body,accessors().vtable,motion);
        const unsigned phase=after_until?3:trial.pending()?(ui.load()==1?1:2):0;
        if(retired.load() || retiring.load()) {status=MotionRead::changed;motion={};}
        record(3,(unsigned(status)<<4)|phase,motion.linear_speed,motion.angular_speed);
    }
    if(!trial.pending() && ((after_until && now>=after_until) || (selected.epoch && now-selected_tick>15000))) {
        selected={};watched_owner=0;retired=true;after_until=0;
    }
    if(guest_mode && !trial.pending()) {
        if(search.pending()) guest_state=2;
        else if(selected.epoch && ui.load()==0 && !retired.load()) guest_state=3;
        else if(!after_until && selected.epoch) guest_state=9;
        else if(!selected.epoch) guest_state=0;
        if(guest_releasing.load() || !keep || (requested&4)) {
            search.cancel();selected={};watched_owner=0;retired=true;after_until=0;ui=-1;
            guest_state=0;guest_owner=0;guest_releasing=false;
        } else if(!selected.epoch && !search.pending()) guest_owner=0;
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
void record_retirement(void* owner,uint64_t body,bool whole_scene) noexcept {
    LifetimeIdentity identity{};
    if(!lifetime_diagnostics.take(token(reinterpret_cast<uintptr_t>(owner)),body,whole_scene,identity)) return;
    Event e{};e.kind=Kind::body;e.edge=6;e.flags=whole_scene?1:0;
    e.qpc=GetTickCount64();e.thread=GetCurrentThreadId();e.span=identity.selection;
    e.object=identity.scene;e.entity=identity.entity;e.detail=identity.body;
    events.push(e);
}
void destroy(void* owner) {
    ++retiring; ++retirement_serial;
    if(token(reinterpret_cast<uintptr_t>(owner))==watched_owner.load()) {retired=true;++retirements;}
    record_retirement(owner,0,true);
    destroy_original(owner);
    ++retirement_serial; --retiring;
}
void release(void* owner,uint64_t body) {
    ++retiring; ++retirement_serial;
    if(token(reinterpret_cast<uintptr_t>(owner))==watched_owner.load() && body==watched_body.load()) {retired=true;++retirements;}
    record_retirement(owner,body,false);
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
std::string Session::start(const std::filesystem::path& root,bool wasm) {
    wasm_=wasm;guest_mode=wasm;
    wchar_t path[32768]{}; const auto size=GetModuleFileNameW(nullptr,path,32768);
    const auto actual_sha=size && size<32768?module_fingerprint(path):std::string{};
    if(!compatibility::allowed(actual_sha)) return "Physics trial refused: unsupported executable fingerprint";
    auto backend=GetModuleHandleW(L"PhysX_64.dll");
    const auto length=backend?GetModuleFileNameW(backend,path,32768):0;
    if(!length || length>=32768 || module_fingerprint(path)!=backend_sha) return "Physics trial refused: unsupported backend fingerprint";
    image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); backend_image=reinterpret_cast<uintptr_t>(backend);
    for(const auto& hook:hooks) if(!compatibility::matches(reinterpret_cast<void*>(image+hook.rva),hook.bytes.data(),hook.bytes.size())) return "Physics trial refused: changed hook entry";
    const auto init=MH_Initialize(); if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return "Physics trial refused: hook initialization";
    if(!probe::input::start()) return "Physics trial refused: keyboard observation unavailable";
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
    header<<"{\"type\":\"header\",\"schema\":1,\"mode\":\"native-physics-trial\",\"sha256\":\""<<actual_sha<<"\",\"physx_sha256\":\""<<backend_sha<<"\",\"mods_suspended\":"<<(wasm?"false":"true")<<"}\n";
    const auto header_text=header.str();log_<<header_text;bytes_=header_text.size();
    log_.flush(); if(!log_) return "Physics trial refused: cannot write log";
    overlay_=probe::overlay_create(image,true,wasm);
    if(!overlay_) return "Physics trial refused: overlay unavailable";
    events.open();
    for(const auto& hook:hooks) if(MH_EnableHook(reinterpret_cast<void*>(image+hook.rva))!=MH_OK) return "Physics trial refused: hook enable";
    accepting=true; ready=true; active_=true;
    return (wasm?"Wasm physics service ready; F11/Esc cancel; log: ":"Native physics trial ready: F9 select, F10 apply for five seconds, F11/Esc restore; log: ")+name;
}
uint32_t Session::input_buttons() noexcept {
    if(!active_ || !wasm_ || !focused() || down(VK_ESCAPE)) return 0;
    return unsigned(down(VK_F7)) | (unsigned(down(VK_F8))<<1);
}
int Session::physics_select(uint64_t owner) noexcept {
    if(!active_ || !wasm_ || !owner || !accepting.load() || !focused() || down(VK_ESCAPE)) return -1;
    StateGuard guard;if(!guard.held) return -2;
    if(guest_releasing.load() || commands.load() || trial.pending() || search.pending()
       || (guest_owner.load() && guest_owner.load()!=owner)) return -2;
    guest_owner=owner;guest_state=1;request_tick=GetTickCount64();commands.fetch_or(1);return 0;
}
uint64_t Session::physics_target(uint64_t owner) noexcept {
    if(!active_ || !wasm_ || !owner) return 0;
    StateGuard guard;if(!guard.held) return 0;
    const auto now=GetTickCount64();
    return guest_owner.load()==owner && !guest_releasing.load() && !commands.load() && !retired.load()
        && !trial.pending() && ui.load()==0 && selected.epoch && now>=selected_tick && now-selected_tick<=15000?selected.epoch:0;
}
int Session::physics_apply(uint64_t owner,uint64_t handle,float value,uint32_t duration) noexcept {
    if(!active_ || !wasm_ || !owner || !accepting.load() || !focused() || down(VK_ESCAPE)) return -1;
    if(!handle || !std::isfinite(value) || value<0 || value>8 || !duration || duration>5000) return -3;
    StateGuard guard;if(!guard.held) return -2;
    if(guest_owner.load()!=owner) return guest_owner.load()?-2:-3;
    if(guest_releasing.load() || commands.load() || trial.pending() || search.pending()) return -2;
    const auto now=GetTickCount64();
    if(handle!=selected.epoch || retired.load() || ui.load()!=0 || now<selected_tick || now-selected_tick>15000) return -3;
    requested_target=handle;requested_damping=value;requested_duration=duration;
    guest_state=1;request_tick=now;commands.fetch_or(2);return 0;
}
int Session::physics_status(uint64_t owner) noexcept {
    if(!active_ || !wasm_ || !owner || !accepting.load()) return -1;
    StateGuard guard;if(!guard.held) return -2;
    if(guest_owner.load()!=owner) return guest_owner.load()?-2:0;
    return guest_releasing.load()?5:guest_state;
}
int Session::physics_restore(uint64_t owner) noexcept {
    if(!active_ || !wasm_ || !owner) return -1;
    if(guest_owner.load()!=owner) return guest_owner.load()?-2:0;
    release(owner);return 0;
}
void Session::release(uint64_t owner) noexcept {
    // Owner cleanup must succeed even when the simulation owns state_lock.
    if(owner && guest_owner.load()==owner) {guest_releasing=true;commands.fetch_or(4);}
}
void Session::poll() {
    if(!active_) return;
    const auto now=GetTickCount64(); heartbeat=now;
    const bool focus=focused();
    const unsigned current=focus?(unsigned(down(VK_F9))|unsigned(down(VK_F10))*2|unsigned(down(VK_F11)||down(VK_ESCAPE))*4):0;
    const auto edges=(current&~keys_)&(wasm_?4u:7u); keys_=current;
    if(now-started_>=600000 || !log_.is_open() || !log_) accepting=false;
    if(!accepting.load() || !focus) commands.fetch_or(4);
    else if(edges && probe::overlay_diagnostics().frames) {
        Event e{};e.kind=Kind::body;e.edge=4;e.flags=edges;e.qpc=now;e.thread=GetCurrentThreadId();events.push(e);
        request_tick=now;commands.fetch_or(edges);
    }
    probe::overlay_update(overlay_,focus,ui.load(),true,down(VK_INSERT));
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
    TargetDiagnostic target{};
    for(unsigned i=0;i<TargetDiagnostics::capacity && log_.is_open() && log_ && target_diagnostics.pop(target);++i) {
        std::ostringstream text;write_target_diagnostic(text,target);const auto line=text.str();
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
            <<",\"target_dropped\":"<<target_diagnostics.dropped()
            <<",\"lifetime_missed\":"<<lifetime_diagnostics.missed()
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
    // Cancellation clears the writable selection; lifecycle evidence must
    // survive it without accessing that state or acquiring its lock.
    watched_owner=0;selected={};events.open();
    lifetime_diagnostics.arm({42,token(0x1000),99,0x500000003ull});
    AcquireSRWLockExclusive(&state_lock);
    release(reinterpret_cast<void*>(0x1000),0x500000003ull);
    ReleaseSRWLockExclusive(&state_lock);
    Event event{};
    ok=ok && events.drain(&event,1)==1 && event.edge==6 && event.flags==0
        && event.span==42 && event.entity==99 && event.detail==0x500000003ull;
    destroy(reinterpret_cast<void*>(0x1000));
    ok=ok && events.drain(&event,1)==0;
    lifetime_diagnostics.arm({43,token(0x2000),100,0x600000004ull});
    destroy(reinterpret_cast<void*>(0x2000));
    ok=ok && events.drain(&event,1)==1 && event.edge==6 && event.flags==1 && event.span==43;
    events.close();destroy_original=old_destroy;release_original=old_release;
    {
        Session service;service.active_=service.wasm_=true;
        accepting=true;test_focus=1;commands=0;guest_owner=0;guest_releasing=false;
        selected={};search.cancel();
        ok=ok && service.physics_select(100)==0 && service.physics_select(200)==-2;
        ok=ok && service.physics_target(200)==0 && service.physics_status(200)==-2;
        commands=0;selected={0x100000001ull,99,0x300000003ull,0};selected_tick=GetTickCount64();ui=0;retired=false;
        ok=ok && service.physics_target(100)==selected.epoch;
        ok=ok && service.physics_apply(200,selected.epoch,8,5000)==-2;
        ok=ok && service.physics_apply(100,1,8,5000)==-3;
        ok=ok && service.physics_apply(100,selected.epoch,8,5001)==-3;
        ok=ok && service.physics_apply(100,selected.epoch,9,5000)==-3;
        ok=ok && service.physics_apply(100,selected.epoch,8,5000)==0;
        ok=ok && requested_target==selected.epoch && requested_duration==5000 && commands.load()==2;
        ok=ok && service.physics_apply(100,selected.epoch,8,5000)==-2;
        service.release(200);ok=ok && !guest_releasing.load();
        AcquireSRWLockExclusive(&state_lock);service.release(100);ReleaseSRWLockExclusive(&state_lock);
        ok=ok && guest_releasing.load() && (commands.load()&4) && service.physics_target(100)==0;
        guest_releasing=false;commands=0;retired=true;
        ok=ok && service.physics_target(100)==0 && service.physics_apply(100,selected.epoch,8,5000)==-3;
        retired=false;selected_tick=GetTickCount64()-16000;
        ok=ok && service.physics_target(100)==0 && service.physics_apply(100,selected.epoch,8,5000)==-3;
        test_focus=0;ok=ok && service.physics_select(100)==-1;
        service.active_=service.wasm_=false;selected={};guest_owner=0;commands=0;test_focus=-1;
    }
    return ok;
}
#endif
}
