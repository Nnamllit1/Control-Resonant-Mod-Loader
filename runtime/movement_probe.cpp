#include "movement_probe.h"
#include "controller_hook.h"
#include "compatibility.h"
#include "movement_view.h"
#include "player_snapshot.h"
#include "navigation_snapshot.h"
#include "diagnostics/entity_inspector.h"
#include "visibility.h"
#include "noclip.h"
#include "overlay.h"
#include "input_filter.h"
#include "fall_guard.h"
#include "diagnostics/fall_observer.h"
#include "boundary_guard.h"
#include "lua_lifetime.h"
#include "lua_probe.h"
#include "lua_session.h"
#include <Windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace crml::probe {
namespace {
uintptr_t image_base{};
std::atomic<bool> recording{};
std::atomic<uint64_t> calls{}, invalid{}, players{}, others{};
std::atomic<uint64_t> overrides{};
std::array<std::atomic<uint64_t>, static_cast<size_t>(Rejection::count)> rejected{};
SRWLOCK sample_lock = SRWLOCK_INIT;
Sample latest{};
EntitySnapshot latest_entity{};
std::atomic<bool> inspecting{};
uint64_t entity_tick{};
DWORD entity_thread{};
DWORD latest_thread{};
uint64_t latest_tick{};
std::array<float,3> last_target{}, last_controller_result{};
uint64_t result_tick{};
bool result_valid{};
Flight flight{};
std::atomic<bool> gameplay_enabled{};
std::atomic<bool> player_read_enabled{};
PlayerSnapshot player_snapshot{}; // Protected by sample_lock, including invalidation.
std::atomic<bool> navigation_enabled{};
navigation::Snapshot navigation_snapshot{}; // Same lock and native publication boundary.
LeaseToggle toggle; // Accessed under sample_lock; survives lease cleanup.
uint64_t last_poll{};
std::atomic<uint64_t> motion_requests{};
bool focused() noexcept {
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
bool down(int key) noexcept { return input::down(static_cast<unsigned>(key)); }

fall::Player active_player() noexcept {
    if(!gameplay_enabled.load(std::memory_order_acquire) || !focused() || !input::fresh() || down(VK_ESCAPE)) return {};
    const auto now=GetTickCount64();
    fall::Player player{};
    AcquireSRWLockShared(&sample_lock);
    if(flight.enabled && flight.owner && flight.entity==latest.entity && flight.world==latest.world &&
       latest_tick && now>=latest_tick && now-latest_tick<=500 && now>=flight.lease && now-flight.lease<=500 &&
       !latest.disabled && !flight.teleport_blocks(latest.teleported) && !latest.keyframed[0] && !latest.keyframed[1])
        player={flight.world,flight.entity};
    ReleaseSRWLockShared(&sample_lock);
    return player;
}
bool input_active() noexcept { return active_player().entity!=0; }

fall::Player observed_player() noexcept {
    // Keep observing after the movement lease is cancelled by a teleport.
    // Never wait on the worker or carry a stale world across a loading gap.
    fall::Player p{};
    if(!TryAcquireSRWLockShared(&sample_lock)) return p;
    const auto now=GetTickCount64();
    if(latest_tick && now>=latest_tick && now-latest_tick<=500) p={latest.world,latest.entity};
    ReleaseSRWLockShared(&sample_lock);
    return p;
}

Sample visibility_player() noexcept {
    Sample p{};
    if(!focused() || down(VK_ESCAPE)) return p;
    AcquireSRWLockShared(&sample_lock);
    const auto now=GetTickCount64();
    if(latest_tick && now>=latest_tick && now-latest_tick<=500) p=latest;
    ReleaseSRWLockShared(&sample_lock);
    return p;
}

Observation observe(void* view, void* world, Sample& sample) noexcept {
    __try {
        const auto tag = *reinterpret_cast<const uint16_t*>(compatibility::address(image_base,0x5c00ca4));
        return inspect(view, world, tag, sample);
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        sample.rejection=Rejection::memory;
        return Observation::invalid;
    }
}

void movement(controller::Move original, void* view, void* world, void* collisions, void* callback, void* scene, void* time) {
    Override replacement;
    bool override_movement = false;
    if (recording.load(std::memory_order_relaxed) || gameplay_enabled.load(std::memory_order_relaxed) || player_read_enabled.load(std::memory_order_relaxed)) {
        calls.fetch_add(1, std::memory_order_relaxed);
        Sample sample{};
        switch (observe(view, world, sample)) {
        case Observation::player:
            fall_trace::controller({sample.world,sample.entity});
            inspect_camera(sample.world,sample.camera);
            players.fetch_add(1, std::memory_order_relaxed);
            // Never fall back to colliding movement merely because the worker is
            // reading diagnostics. All holders release before calling game code.
            AcquireSRWLockExclusive(&sample_lock);
            {
                const auto now=GetTickCount64();
                if(inspecting.load(std::memory_order_relaxed) && (!entity_tick || now-entity_tick>=1000)) {
                    inspect_entity(sample,latest_entity);
                    entity_tick=now; entity_thread=GetCurrentThreadId();
                }
                latest = sample;
                latest_thread = GetCurrentThreadId();
                latest_tick = GetTickCount64();
                if(player_read_enabled.load(std::memory_order_relaxed)) player_snapshot.publish(sample,latest_tick);
                if(navigation_enabled.load(std::memory_order_relaxed)) {
                    navigation::GroundObservation ground{};
                    const auto status=navigation::inspect_ground(view,sample.world,sample.entity,ground);
                    navigation_snapshot.publish(sample,status==navigation::GroundStatus::valid?&ground:nullptr,latest_tick);
                }
                if (gameplay_enabled.load(std::memory_order_relaxed)) {
                    std::array<float,3> target{};
                    const Direction keys{float(down('D'))-float(down('A')), float(down(VK_SPACE))-float(down(VK_CONTROL)), float(down('W'))-float(down('S')), down(VK_SHIFT)};
                    const auto direction=flight.guest_driven?flight.requested:camera_relative(keys,sample.camera);
                    if(down(VK_ESCAPE)) flight.reset(StopReason::escape,latest_tick,&sample);
                    if (flight.step(sample, sample.world, latest_tick, focused(), input::fresh(), direction, target)) {
                        override_movement = replacement.prepare(view, sample, target);
                        if (override_movement) overrides.fetch_add(1, std::memory_order_relaxed);
                        if (!override_movement) flight.reset(StopReason::view,latest_tick,&sample);
                    }
                }
                ReleaseSRWLockExclusive(&sample_lock);
            }
            break;
        case Observation::other_entity: others.fetch_add(1, std::memory_order_relaxed); break;
        default:
            if(player_read_enabled.load(std::memory_order_relaxed)) {
                AcquireSRWLockExclusive(&sample_lock);
                player_snapshot.invalidate();
                navigation_snapshot.invalidate();
                ReleaseSRWLockExclusive(&sample_lock);
            }
            invalid.fetch_add(1, std::memory_order_relaxed);
            rejected[static_cast<size_t>(sample.rejection)].fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    // Keep all six arguments intact. Do not intercept the game's exceptions.
    original(override_movement ? replacement.view.data() : view, world, collisions, callback, scene, time);
    if(override_movement) {
        Sample after{};
        const bool valid=observe(view,world,after)==Observation::player;
        AcquireSRWLockExclusive(&sample_lock);
        for(int i=0;i<3;++i) { last_target[i]=replacement.transform[4+i]; last_controller_result[i]=valid?after.controller_position[i]:0.f; }
        result_tick=GetTickCount64(); result_valid=valid;
        ReleaseSRWLockExclusive(&sample_lock);
    }
}

std::string fingerprint(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::array<unsigned char, 65536> buffer{};
    bool ok = true;
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (file.gcount() && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0) < 0) { ok = false; break; }
    }
    std::array<unsigned char, 32> digest{};
    ok = ok && file.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream text;
    for (auto byte : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return text.str();
}
}

std::string Recorder::start(const std::filesystem::path& root,uint32_t services) {
    const bool navigation_requested=(services&CRML_CAP_NAVIGATION_READ) && compatibility::reviewed_build &&
        compatibility::engine_profile==compatibility::EngineProfile::october_patch;
    const bool read_requested=(services&CRML_CAP_PLAYER_READ)!=0 || navigation_requested;
    const bool motion_requested=services ? (services&(CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION))!=0 : std::filesystem::is_regular_file(root / "movement-wasm.enabled");
    const bool visibility_requested=(services&CRML_CAP_PLAYER_VISIBILITY)!=0 || (!services && std::filesystem::is_regular_file(root / "visibility.enabled"));
    const bool inspector_requested=!services && !motion_requested && std::filesystem::is_regular_file(root / "entity-inspector.enabled");
    const bool noclip_requested = !motion_requested && !inspector_requested && ((services&CRML_CAP_PLAYER_NOCLIP)!=0 || (!services && std::filesystem::is_regular_file(root / "noclip.enabled")));
    if (!read_requested && !motion_requested && !visibility_requested && !inspector_requested && !noclip_requested && !std::filesystem::is_regular_file(root / "movement-probe.enabled")) return "Experimental gameplay and movement probe disabled";
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    const auto actual_sha=length && length<32768?fingerprint(executable):std::string{};
    if (!compatibility::allowed(actual_sha))
        return "Movement probe refused: unsupported executable fingerprint";
    image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const bool read_only=read_requested && !motion_requested && !visibility_requested && !inspector_requested && !noclip_requested;
    if(!read_only || std::filesystem::is_regular_file(root / "movement-probe.enabled"))
        output_.open(root / "movement-probe.jsonl", std::ios::trunc);
    if (!output_.is_open() && !read_requested) return "Movement probe refused: cannot open diagnostic log";
    if(inspector_requested) {
        entity_output_.open(root / "entity-inspector.jsonl",std::ios::trunc);
        if(!entity_output_) { output_.close(); return "Entity inspector refused: cannot open log"; }
        entity_output_ << "{\"visibility_requested\":" << (visibility_requested?"true":"false") << ",\"schema\":1,\"mode\":\"player-inspector\",\"phase\":\"before-controller-update\",\"sha256\":\""<<actual_sha<<"\"}\n";
        inspecting.store(true);
    }
    if (!controller::start(image_base) || !controller::movement(&movement)) {
        recording.store(false);
        inspecting.store(false); entity_output_.close();
        output_.close();
        return "Movement service refused: controller hook unavailable";
    }
    read_=read_requested;
    navigation_=navigation_requested;
    navigation_enabled.store(navigation_,std::memory_order_release);
    player_read_enabled.store(read_,std::memory_order_release);
    visibility_ = visibility_requested && visibility::start(image_base,&visibility_player);
    motion_=motion_requested && input::start(&input_active);
    gameplay_ = motion_ || (noclip_requested && fall::start(image_base,&active_player) && input::start(&input_active));
    if(visibility_ && !input::start()) {visibility::stop();visibility_=false;}
    recording.store(output_.is_open() || visibility_, std::memory_order_release);
    gameplay_enabled.store(gameplay_);
    bool fall_observer=false,boundary_guard=false;
    if(motion_) {
        fall_output_.open(root / "fall-recovery.jsonl",std::ios::trunc);
        fall_observer=fall_output_.is_open() && fall_trace::start(image_base,&observed_player);
        boundary_guard=fall_observer && boundary::start(image_base,&active_player);
        if(fall_output_) {
            fall_output_<<"{\"schema\":4,\"mode\":\""<<(boundary_guard?"flight-boundary-guard":"observe-only")<<"\",\"active\":"<<(fall_observer?"true":"false")
                        <<",\"sha256\":\""<<actual_sha<<"\"}\n";
            fall_output_.flush();
        }
        if(!fall_observer) fall_output_.close();
    }
    if (gameplay_) overlay_ = overlay_create(image_base,false,false,motion_);
    output_ << "{\"schema\":7,\"mode\":\"" << (motion_?"wasm-movement":visibility_ ? "experimental-visibility" : gameplay_ ? "experimental-noclip" : "observe-only") << "\",\"pid\":" << GetCurrentProcessId() << "}\n";
    output_.flush();
    if(motion_requested) return motion_ ? (boundary_guard ?
        "Wasm movement service armed; scoped boundary guard and recovery trace active" :
        "Wasm movement service armed; boundary guard unavailable; engine recovery remains enabled") :
        "Wasm movement refused: input hook unavailable; observer remains read-only";
    if(noclip_requested && !gameplay_) return "Experimental noclip refused: input or fall-recovery hook unavailable; movement probe remains read-only";
    if(visibility_requested) return visibility_?"Experimental player visibility armed; requests controlled by player.visibility Wasm mods":"Visibility hook refused; observer remains active";
    if(inspector_requested) return "Read-only player entity inspector active; noclip disabled for this session";
    if(read_) return "Read-only player state service ready";
    return gameplay_ ? "Experimental noclip bridge armed; requires player.noclip Wasm mod and F6. Live gameplay unverified."
                     : "Movement probe active; observing controller calls without changing gameplay";
}

void Recorder::poll() {
    if (gameplay_) {
        const auto now = GetTickCount64();
        const bool foreground = focused();
        AcquireSRWLockExclusive(&sample_lock);
        if (!foreground) flight.reset(StopReason::focus,now,&latest);
        else if(down(VK_ESCAPE)) flight.reset(StopReason::escape,now,&latest);
        else if(!input::fresh() || !latest_tick || now<latest_tick || now-latest_tick>500) flight.reset(StopReason::stale_sample,now,&latest);
        else if(flight.enabled && now-flight.lease>500) flight.reset(StopReason::lease,now,&latest);
        const int state = !foreground || !input::fresh() || !latest_tick || now<latest_tick || now-latest_tick > 500 || !last_poll || now-last_poll > 500 || latest.disabled || flight.teleport_blocks(latest.teleported) || latest.keyframed[0] || latest.keyframed[1] ||
                          (!motion_ && !flight.enabled && !fall::available({latest.world,latest.entity})) ? -1 : flight.enabled ? 1 : 0;
        const bool camera_valid=latest.camera.valid;
        ReleaseSRWLockExclusive(&sample_lock);
        overlay_update(overlay_, foreground, state, camera_valid, down(VK_INSERT));
    }
    if (!output_.is_open() || ++polls_ % (motion_?100:10)) return;
    if(fall_output_.is_open()) {
        fall_trace::write(fall_output_);boundary::write(fall_output_);fall_output_.flush();
        if(!fall_output_) {lua::stop();lua::lifetime::stop();fall_trace::stop();fall_output_.close();}
    }
    Sample sample{};
    DWORD thread{};
    uint64_t tick{};
    bool enabled{};
    Direction guest_velocity{};
    std::array<float,3> target{}, controller_result{};
    uint64_t outcome_tick{};
    bool outcome_valid{};
    FlightStop stopped{};
    TeleportRestore restored{};
    EntitySnapshot entity_snapshot{};
    uint64_t snapshot_tick{};
    DWORD snapshot_thread{};
    AcquireSRWLockShared(&sample_lock);
    sample = latest;
    thread = latest_thread;
    tick = latest_tick;
    enabled = flight.enabled;
    if(flight.guest_driven) guest_velocity={flight.requested.x*20.f,flight.requested.y*20.f,flight.requested.z*20.f,false};
    stopped=flight.stopped;
    restored=flight.restored;
    entity_snapshot=latest_entity; snapshot_tick=entity_tick; snapshot_thread=entity_thread;
    target=last_target; controller_result=last_controller_result; outcome_tick=result_tick; outcome_valid=result_valid;
    ReleaseSRWLockShared(&sample_lock);
    if(entity_output_ && snapshot_tick && snapshot_tick!=last_entity_report_) {
        write_entity_snapshot(entity_output_,entity_snapshot,snapshot_tick,snapshot_thread);
        entity_output_.flush(); last_entity_report_=snapshot_tick;
        if(!entity_output_) inspecting.store(false);
    }
    const auto graphics=overlay_diagnostics();
    output_ << "{\"calls\":" << calls.load() << ",\"invalid\":" << invalid.load()
            << ",\"visibility_submissions\":" << visibility::submissions() << ",\"other_entities\":" << others.load() << ",\"player_samples\":" << players.load()
            << ",\"overrides\":" << overrides.load() << ",\"noclip_active\":" << (enabled ? "true" : "false")
            << ",\"motion_requests\":" << motion_requests.load()
            << ",\"motion_velocity\":[" << guest_velocity.x << ',' << guest_velocity.y << ',' << guest_velocity.z << ']'
            << ",\"input_consumed\":" << input::consumed() << ",\"fall_checks_skipped\":" << fall::skipped_checks()
            << ",\"boundary_targets_skipped\":" << fall::skipped_triggers()
            << ",\"fall_monitors_skipped\":" << fall::skipped_monitors() << ",\"fall_actions_skipped\":" << fall::skipped_fall_actions()
            << ",\"active_recoveries_skipped\":" << fall::skipped_active_recoveries()
            << ",\"fall_camera_overrides\":" << fall::camera_overrides() << ",\"fall_camera_clear_requests\":" << fall::camera_clear_requests()
            << ",\"thread\":" << thread << ",\"sample_age_ms\":" << (tick ? GetTickCount64() - tick : 0)
            << ",\"entity\":" << sample.entity << ",\"row\":" << sample.row;
    output_ << ",\"overlay\":{\"status\":\"" << graphics.status << "\",\"presents\":" << graphics.presents
            << ",\"frames\":" << graphics.frames << ",\"queue_matches\":" << graphics.queue_matches << "},\"rejections\":{";
    for(size_t i=0;i<rejected.size();++i) { if(i) output_ << ','; output_ << '\"' << rejection_names[i] << "\":" << rejected[i].load(); }
    output_ << '}';
    output_ << ",\"last_stop\":{\"reason\":\"" << stop_names[static_cast<size_t>(stopped.reason)]
            << "\",\"count\":" << stopped.count << ",\"tick_ms\":" << stopped.tick << ",\"entity\":" << stopped.entity
            << ",\"source\":\"" << stopped.source << "\",\"sample_age_ms\":" << stopped.sample_age_ms
            << std::setprecision(9) << ",\"requested\":[" << stopped.requested[0] << ',' << stopped.requested[1] << ',' << stopped.requested[2]
            << "],\"observed\":[" << stopped.observed[0] << ',' << stopped.observed[1] << ',' << stopped.observed[2] << "]}";
    output_ << ",\"teleport_restores\":{\"count\":" << restored.count << ",\"tick_ms\":" << restored.tick
            << ",\"requested\":[" << restored.requested[0] << ',' << restored.requested[1] << ',' << restored.requested[2]
            << "],\"observed\":[" << restored.observed[0] << ',' << restored.observed[1] << ',' << restored.observed[2] << "]}";
    output_ << ",\"camera_valid\":" << (sample.camera.valid?"true":"false") << ",\"camera_entity\":" << sample.camera.entity
            << ",\"teleported\":" << unsigned(sample.teleported)
            << ",\"last_override\":{\"valid\":" << (outcome_valid?"true":"false") << ",\"age_ms\":" << (outcome_tick?GetTickCount64()-outcome_tick:0)
            << std::setprecision(9) << ",\"target\":[" << target[0] << ',' << target[1] << ',' << target[2]
            << "],\"controller_result\":[" << controller_result[0] << ',' << controller_result[1] << ',' << controller_result[2] << "]}";
    output_ << std::setprecision(9) << ",\"position\":[" << sample.position[0] << ',' << sample.position[1] << ',' << sample.position[2]
            << "],\"controller_position\":[" << sample.controller_position[0] << ',' << sample.controller_position[1] << ',' << sample.controller_position[2]
            << "],\"keyframed\":[" << unsigned(sample.keyframed[0]) << ',' << unsigned(sample.keyframed[1])
            << "],\"disabled\":" << unsigned(sample.disabled) << "}\n";
    output_.flush();
    // Bound recording time and disk use. The pinned trampoline remains a pass-through until exit.
    if (!output_ || ++reports_ >= 600) {
        if(!visibility_) recording.store(false);
        inspecting.store(false); entity_output_.close();
        output_.close();
        lua::stop();lua::lifetime::stop();fall_trace::stop();
        if(fall_output_) {lua::lifetime::write(fall_output_);lua::session::write(fall_output_);fall_output_.flush();}
        fall_output_.close();
    }
}

uint32_t Recorder::capabilities() const noexcept {
    return (motion_ ? CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION : 0u)
         | (gameplay_ && !motion_ ? CRML_CAP_PLAYER_NOCLIP : 0u)
         | (visibility_ ? CRML_CAP_PLAYER_VISIBILITY : 0u)
         | (read_ ? CRML_CAP_PLAYER_READ : 0u)
         | (navigation_ ? CRML_CAP_NAVIGATION_READ : 0u)
         | (input::observing() ? CRML_CAP_INPUT_BUTTONS : 0u);
}
int Recorder::player_read(crml_player_state& out) noexcept {
    out={};
    if(!read_) return -1;
    const auto now=GetTickCount64();
    const bool foreground=focused();
    AcquireSRWLockShared(&sample_lock);
    const auto result=player_snapshot.read(out,now,foreground);
    ReleaseSRWLockShared(&sample_lock);
    return result;
}
int Recorder::navigation_read(crml_navigation_state& out) noexcept {
    out={};if(!navigation_)return -1;
    const auto now=GetTickCount64();const bool foreground=focused();
    AcquireSRWLockShared(&sample_lock);
    const auto result=navigation_snapshot.read(out,now,foreground);
    ReleaseSRWLockShared(&sample_lock);return result;
}
int Recorder::navigation_read_v2(crml_navigation_state_v2& out) noexcept {
    out={};if(!navigation_)return -1;
    const auto now=GetTickCount64();const bool foreground=focused();
    AcquireSRWLockShared(&sample_lock);
    const auto result=navigation_snapshot.read_v2(out,now,foreground);
    ReleaseSRWLockShared(&sample_lock);return result;
}
int Recorder::noclip_poll(uint64_t owner, float speed) noexcept {
    if (!gameplay_ || motion_ || !std::isfinite(speed) || speed < .25f || speed > 20.f) return -1;
    const bool key = focused() && down(VK_F6);
    const auto now = GetTickCount64();
    AcquireSRWLockExclusive(&sample_lock);
    int result = 0;
    bool activated=false;
    const int edge=toggle.sample(owner,flight.owner,key);
    if(edge<0) result=edge;
    else if (!focused() || !input::fresh() || !latest_tick || now<latest_tick || now-latest_tick > 500 || latest.disabled || flight.teleport_blocks(latest.teleported) || latest.keyframed[0] || latest.keyframed[1] ||
        (!flight.enabled && !fall::available({latest.world,latest.entity}))) {
        const auto reason=!focused()?StopReason::focus:!input::fresh() || !latest_tick || now<latest_tick || now-latest_tick>500?StopReason::stale_sample:
                          latest.disabled?StopReason::disabled:latest.teleported?StopReason::teleport:StopReason::keyframed;
        flight.reset(reason,now,&latest); result = -1;
    } else {
        last_poll = now;
        if (edge==1) {
            if (flight.enabled) flight.reset(StopReason::toggle,now,&latest);
            else {
                flight.owner=owner; flight.entity=latest.entity; flight.world=latest.world;
                flight.enabled=true; flight.last_step=0;
                activated=true;
            }
        }
        if (flight.enabled) { flight.lease=now; flight.speed=speed; result=1; }
    }
    ReleaseSRWLockExclusive(&sample_lock);
    if(activated) input::release_held(GetForegroundWindow());
    return result;
}

uint32_t Recorder::input_buttons() noexcept {
    if(!focused() || down(VK_ESCAPE)) return 0;
    return (down(VK_F7)?1u:0u) | (down(VK_F8)?2u:0u);
}
uint32_t Recorder::input_motion() noexcept {
    if(!motion_ || !focused() || down(VK_ESCAPE)) return 0;
    const int keys[]{VK_F6,'W','S','A','D',VK_SPACE,VK_CONTROL,VK_SHIFT};
    uint32_t mask{};for(unsigned i=0;i<8;++i) if(down(keys[i])) mask|=1u<<i;
    return mask;
}
int Recorder::motion_camera(float (&right)[2]) noexcept {
    right[0]=right[1]=0;
    if(!motion_ || !focused() || down(VK_ESCAPE)) return -1;
    const auto now=GetTickCount64();
    AcquireSRWLockShared(&sample_lock);
    const auto camera=latest.camera;
    const bool valid=latest_tick && now>=latest_tick && now-latest_tick<=100 && camera.valid;
    ReleaseSRWLockShared(&sample_lock);
    const auto length=std::hypot(camera.right[0],camera.right[2]);
    if(!valid || !std::isfinite(length) || length<.01f) return -1;
    right[0]=camera.right[0]/length;right[1]=camera.right[2]/length;return 1;
}
int Recorder::motion_read(uint64_t owner,crml_motion_state& out) noexcept {
    out={};if(!motion_ || !owner) return -1;
    // A cancellation remains readable without focus or a current player sample.
    AcquireSRWLockShared(&sample_lock);
    const int result=flight.read_motion(owner,out);
    ReleaseSRWLockShared(&sample_lock);
    return result;
}
int Recorder::motion_set(uint64_t owner,bool enable,float x,float y,float z) noexcept {
    if(!motion_ || !owner) return -1;
    bool activated=false;
    AcquireSRWLockExclusive(&sample_lock);
    const auto now=GetTickCount64();
    const bool was_enabled=flight.enabled;
    const int result=flight.request_sampled_motion(owner,enable,x,y,z,latest,latest_tick,now,focused(),down(VK_ESCAPE),input::fresh());
    activated=result==1 && !was_enabled;
    if(result>=0) {last_poll=now;++motion_requests;}
    ReleaseSRWLockExclusive(&sample_lock);
    if(activated) input::release_held(GetForegroundWindow());
    return result;
}

int Recorder::visibility_set(uint64_t owner,bool hidden) noexcept {
    if(!visibility_) return -1;
    return visibility::poll(owner,hidden && focused() && !down(VK_ESCAPE),GetTickCount64());
}
int Recorder::visibility_read(uint64_t owner,crml_visibility_state& out) noexcept {
    out={};if(!visibility_) return -1;
    return visibility::read_state(owner,GetTickCount64(),out);
}

int Recorder::visibility_poll(uint64_t owner) noexcept {
    if(!visibility_) return -1;
    return visibility::poll(owner,focused() && !down(VK_ESCAPE) && down(VK_F7),GetTickCount64());
}

void Recorder::release(uint64_t owner) noexcept {
    visibility::release(owner);
    AcquireSRWLockExclusive(&sample_lock);
    if (flight.owner == owner) last_poll = 0;
    flight.release_owner(owner,GetTickCount64(),&latest);
    ReleaseSRWLockExclusive(&sample_lock);
}

Recorder::~Recorder() {
    boundary::stop();
    fall_trace::stop();
    if(fall_output_.is_open()) {fall_trace::write(fall_output_);boundary::write(fall_output_);fall_output_.flush();}
    visibility::stop();
    inspecting.store(false);
    recording.store(false); gameplay_enabled.store(false); player_read_enabled.store(false);navigation_enabled.store(false);
    AcquireSRWLockExclusive(&sample_lock); player_snapshot.invalidate(); navigation_snapshot.invalidate(); flight.reset(StopReason::shutdown,GetTickCount64(),&latest); ReleaseSRWLockExclusive(&sample_lock);
    overlay_destroy(overlay_);
}
}
