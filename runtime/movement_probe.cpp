#include "movement_probe.h"
#include "movement_view.h"
#include "entity_inspector.h"
#include "noclip.h"
#include "overlay.h"
#include "input_filter.h"
#include "fall_guard.h"
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
using Move = void(*)(void*, void*, void*, void*, void*, void*);
Move original{};
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
bool toggle_down{}; // Runtime worker only.
uint64_t last_poll{};
bool focused() noexcept {
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
bool down(int key) noexcept { return (GetAsyncKeyState(key) & 0x8000) != 0; }

fall::Player active_player() noexcept {
    if(!gameplay_enabled.load(std::memory_order_acquire) || !focused() || down(VK_ESCAPE)) return {};
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

Observation observe(void* view, void* world, Sample& sample) noexcept {
    __try {
        const auto tag = *reinterpret_cast<const uint16_t*>(image_base + 0x5c00ca4);
        return inspect(view, world, tag, sample);
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        sample.rejection=Rejection::memory;
        return Observation::invalid;
    }
}

void movement(void* view, void* world, void* collisions, void* callback, void* scene, void* time) {
    Override replacement;
    bool override_movement = false;
    if (recording.load(std::memory_order_relaxed) || gameplay_enabled.load(std::memory_order_relaxed)) {
        calls.fetch_add(1, std::memory_order_relaxed);
        Sample sample{};
        switch (observe(view, world, sample)) {
        case Observation::player:
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
                if (gameplay_enabled.load(std::memory_order_relaxed)) {
                    std::array<float,3> target{};
                    const Direction keys{float(down('D'))-float(down('A')), float(down(VK_SPACE))-float(down(VK_CONTROL)), float(down('W'))-float(down('S')), down(VK_SHIFT)};
                    const auto direction=camera_relative(keys,sample.camera);
                    if(down(VK_ESCAPE)) flight.reset(StopReason::escape,latest_tick,&sample);
                    if (flight.step(sample, sample.world, latest_tick, focused(), direction, target)) {
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

std::string Recorder::start(const std::filesystem::path& root) {
    const bool inspector_requested=std::filesystem::is_regular_file(root / "entity-inspector.enabled");
    const bool noclip_requested = !inspector_requested && std::filesystem::is_regular_file(root / "noclip.enabled");
    if (!inspector_requested && !noclip_requested && !std::filesystem::is_regular_file(root / "movement-probe.enabled")) return "Experimental gameplay and movement probe disabled";
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768 || fingerprint(executable) != "2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0")
        return "Movement probe refused: unsupported executable fingerprint";
    image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto target = reinterpret_cast<void*>(image_base + 0x1b98950);
    constexpr unsigned char signature[] = {0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x53,0x56,0x57};
    if (std::memcmp(target, signature, sizeof(signature)) != 0) return "Movement probe refused: movement routine changed or already hooked";
    output_.open(root / "movement-probe.jsonl", std::ios::trunc);
    if (!output_) return "Movement probe refused: cannot open diagnostic log";
    if(inspector_requested) {
        entity_output_.open(root / "entity-inspector.jsonl",std::ios::trunc);
        if(!entity_output_) { output_.close(); return "Entity inspector refused: cannot open log"; }
        entity_output_ << "{\"schema\":1,\"mode\":\"read-only-player\",\"phase\":\"before-controller-update\",\"sha256\":\"2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0\"}\n";
        inspecting.store(true);
    }
    auto status = MH_Initialize();
    if (status == MH_OK) status = MH_CreateHook(target, reinterpret_cast<void*>(&movement), reinterpret_cast<void**>(&original));
    if (status == MH_OK) {
        recording.store(true, std::memory_order_release);
        status = MH_EnableHook(target);
    }
    if (status != MH_OK) {
        recording.store(false);
        inspecting.store(false); entity_output_.close();
        output_.close();
        return std::string("Movement probe refused: ") + MH_StatusToString(status);
    }
    gameplay_ = noclip_requested && fall::start(image_base,&active_player) && input::start(&input_active);
    gameplay_enabled.store(gameplay_);
    if (gameplay_) overlay_ = overlay_create();
    output_ << "{\"schema\":7,\"mode\":\"" << (gameplay_ ? "experimental-noclip" : "observe-only") << "\",\"pid\":" << GetCurrentProcessId() << "}\n";
    output_.flush();
    if(noclip_requested && !gameplay_) return "Experimental noclip refused: input or fall-recovery hook unavailable; movement probe remains read-only";
    if(inspector_requested) return "Read-only player entity inspector active; noclip disabled for this session";
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
        else if(!latest_tick || now-latest_tick>500) flight.reset(StopReason::stale_sample,now,&latest);
        else if(flight.enabled && now-flight.lease>500) flight.reset(StopReason::lease,now,&latest);
        const int state = !foreground || !latest_tick || now-latest_tick > 500 || !last_poll || now-last_poll > 500 || latest.disabled || flight.teleport_blocks(latest.teleported) || latest.keyframed[0] || latest.keyframed[1] ||
                          (!flight.enabled && !fall::available({latest.world,latest.entity})) ? -1 : flight.enabled ? 1 : 0;
        const bool camera_valid=latest.camera.valid;
        ReleaseSRWLockExclusive(&sample_lock);
        overlay_update(overlay_, foreground, state, camera_valid);
    }
    if (!output_.is_open() || ++polls_ % 10) return;
    Sample sample{};
    DWORD thread{};
    uint64_t tick{};
    bool enabled{};
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
            << ",\"other_entities\":" << others.load() << ",\"player_samples\":" << players.load()
            << ",\"overrides\":" << overrides.load() << ",\"noclip_active\":" << (enabled ? "true" : "false")
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
        recording.store(false);
        inspecting.store(false); entity_output_.close();
        output_.close();
    }
}

int Recorder::noclip_poll(uint64_t owner, float speed) noexcept {
    if (!gameplay_ || !std::isfinite(speed) || speed < .25f || speed > 20.f) return -1;
    const bool key = focused() && down(VK_F6);
    const bool pressed = key && !toggle_down;
    toggle_down = key;
    const auto now = GetTickCount64();
    AcquireSRWLockExclusive(&sample_lock);
    int result = 0;
    bool activated=false;
    if (!focused() || !latest_tick || now-latest_tick > 500 || latest.disabled || flight.teleport_blocks(latest.teleported) || latest.keyframed[0] || latest.keyframed[1] ||
        (!flight.enabled && !fall::available({latest.world,latest.entity}))) {
        const auto reason=!focused()?StopReason::focus:!latest_tick || now-latest_tick>500?StopReason::stale_sample:
                          latest.disabled?StopReason::disabled:latest.teleported?StopReason::teleport:StopReason::keyframed;
        flight.reset(reason,now,&latest); result = -1;
    } else if (flight.owner && flight.owner != owner) result = -2;
    else {
        last_poll = now;
        if (pressed) {
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

void Recorder::release(uint64_t owner) noexcept {
    AcquireSRWLockExclusive(&sample_lock);
    if (flight.owner == owner) { flight.reset(StopReason::mod_release,GetTickCount64(),&latest); last_poll = 0; }
    ReleaseSRWLockExclusive(&sample_lock);
}

Recorder::~Recorder() {
    inspecting.store(false);
    recording.store(false); gameplay_enabled.store(false);
    AcquireSRWLockExclusive(&sample_lock); flight.reset(StopReason::shutdown,GetTickCount64(),&latest); ReleaseSRWLockExclusive(&sample_lock);
    overlay_destroy(overlay_);
}
}
