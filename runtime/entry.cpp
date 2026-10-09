#include "runtime.h"
#include "compatibility.h"
#include "session_log.h"
#include "session_file.h"
#include "mod_storage.h"
#include "mod_settings.h"
#include "mod_feedback.h"
#include "mod_tutorials.h"
#include "mod_drawing.h"
#include "mod_lists.h"
#ifdef CRML_LUA_SOURCE
#include "lua_source.h"
#endif
#ifdef CRML_MOVEMENT_PROBE
#include "movement_probe.h"
#include "diagnostics/engine_observer.h"
#include "diagnostics/input_context.h"
#include "diagnostics/menu_context.h"
#include "diagnostics/tutorial_observer.h"
#include "diagnostics/navigation_observer.h"
#include "diagnostics/dialogue_observer.h"
#include "diagnostics/action_restriction_observer.h"
#include "diagnostics/map_observer.h"
#include "diagnostics/sonar_observer.h"
#include "tutorial_native.h"
#include "tutorial_hint.h"
#include "physics_session.h"
#include "input_service.h"
#include "gameplay_router.h"
#include "action_rules.h"
#include "camera_service.h"
#include "diagnostics/native_ui.h"
#include "ui_service.h"
#include "media_service.h"
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>

extern "C" __declspec(dllexport) DWORD WINAPI crml_run() {
    static std::once_flag once;
    DWORD result = ERROR_ALREADY_INITIALIZED;
    std::call_once(once, [&] {
        result = 1;
        try {
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(&crml_run), &self)) return;
            wchar_t filename[32768]{};
            const auto size = GetModuleFileNameW(self, filename, 32768);
            if (!size || size >= 32768) return;
            const auto root = std::filesystem::path(filename).parent_path();
            const bool observe_only=std::filesystem::is_regular_file(root / "engine-observer.enabled");
            const bool physics_trial=std::filesystem::is_regular_file(root / "physics-trial.enabled");
            const bool physics_wasm=std::filesystem::is_regular_file(root / "physics-wasm.enabled");
            const bool movement_wasm=std::filesystem::is_regular_file(root / "movement-wasm.enabled");
            const bool diagnostics=observe_only || physics_trial;
            const bool conflict=(observe_only && physics_trial) || (diagnostics && (physics_wasm || movement_wasm));
            crml::SessionFile session_file(root);
            if (!session_file.available()) return;
            auto& log=session_file.stream();
            crml::SessionLog session_log(log);
#ifdef CRML_MOVEMENT_PROBE
            crml::probe::Recorder probe;
            crml::observer::Recorder observer;
            crml::physics::Session physics;
            crml::KeyboardInput keyboard;
            const bool input_context_trace=std::filesystem::is_regular_file(root/"input-context.enabled");
            const bool tutorial_trace=std::filesystem::is_regular_file(root/"tutorial-observer.enabled");
            const bool capability_trace=std::filesystem::is_regular_file(root/"engine-capabilities.enabled");
            crml::camera::Service camera;
            auto& ui=crml::ui::process_service();
            auto& media=crml::media::process_service();
            crml::GameplayRouter services(probe,physics,&camera,&ui,&media,&crml::action_rules::process_service());
#endif
            crml::ModStorage storage(root / "data");
            if(!storage.available()) session_log.host("Mod persistence unavailable: data directory is blocked, linked or already in use");
            crml::Runtime runtime([&](const std::string& text) {
                session_log.host(text);
            }
#ifdef CRML_MOVEMENT_PROBE
            , diagnostics || conflict ? nullptr : &services, &keyboard
#else
            , nullptr, nullptr
#endif
            , [&](std::string_view id,int level,std::string_view text) {session_log.guest(id,level,text);}
            , &storage, &crml::process_settings(), &crml::process_feedback(), {}, &crml::process_tutorials(), &crml::process_drawing(), &crml::process_lists()
            );
            log << "CRML " << crml::runtime_version() << " bootstrap\n";
            if(observe_only) log << "Wasm mods suspended for engine observation\n";
            else if(physics_trial) log << "Wasm mods suspended for native physics trial\n";
            log.flush();
            if(!crml::compatibility::authorize(root,log)) {result=0;return;}
#ifdef CRML_MOVEMENT_PROBE
            struct NativeUiCleanup { ~NativeUiCleanup(){crml::action_rules::stop();crml::action_restriction_observer::stop();crml::navigation_observer::stop();crml::dialogue_observer::stop();crml::tutorial_native::stop();crml::tutorial::shutdown_hint();crml::tutorial_observer::stop();crml::native_ui::stop();crml::media::stop();} } native_ui_cleanup;
            const bool input_context_active=input_context_trace && !conflict && crml::input_context::start();
            const bool menu_context_active=input_context_trace && !conflict && crml::menu_context::start();
            const bool tutorial_active=tutorial_trace && !conflict && crml::tutorial_observer::start();
            const bool location_active=capability_trace && !diagnostics && !conflict && crml::navigation_observer::start();
            const bool dialogue_active=capability_trace && !diagnostics && !conflict && crml::dialogue_observer::start();
            const bool restriction_active=capability_trace && !diagnostics && !conflict && crml::action_restriction_observer::start();
            const bool map_active=(movement_wasm || capability_trace) && !diagnostics && !conflict && crml::map_observer::start(
                [](const crml::map_projection::District& district,const std::array<float,4>& rectangle,uint64_t) noexcept {
                    crml::process_drawing().update_map(district,rectangle);
                },capability_trace);
            struct MapCleanup { ~MapCleanup(){crml::map_observer::stop();} } map_cleanup;
            const bool sonar_active=movement_wasm && !diagnostics && !conflict && crml::sonar_observer::start(
                [](const crml::sonar_projection::Snapshot& snapshot,uint64_t) noexcept {
                    crml::process_drawing().update_sonar(snapshot);
                });
            struct SonarCleanup { ~SonarCleanup(){crml::sonar_observer::stop();} } sonar_cleanup;
            if(movement_wasm)crml::sonar_observer::report_startup(log);
            if(capability_trace) {
                session_log.host(location_active?"Read-only location capture ready":"Location capture unavailable for this build or startup mode");
                session_log.host(dialogue_active?"Read-only dialogue capture ready":"Dialogue capture unavailable for this build or startup mode");
                session_log.host(restriction_active?"Read-only action restriction capture ready":"Action restriction capture unavailable for this build or startup mode");
                session_log.host(map_active?"Read-only map projection capture ready":"Map projection capture unavailable for this build or startup mode");
                crml::map_observer::report_startup(log);
            }
            if(tutorial_trace) session_log.host(tutorial_active?
                "Read-only tutorial observation ready (ten-minute capture)":
                "Tutorial observation unavailable for this build, hook or startup mode");
            if(input_context_trace) session_log.host(input_context_active?
                "Read-only native input context observation ready":
                "Native input context observation unavailable for this build, hook or startup mode");
            if(input_context_trace) session_log.host(menu_context_active?
                "Read-only native menu context observation ready":
                "Native menu context observation unavailable for this build, hook or startup mode");
#endif
#ifdef CRML_LUA_SOURCE
            const bool source_requested=movement_wasm && !conflict && crml::engine::lua::source::prepare(root);
            if(!source_requested && std::filesystem::is_regular_file(root/"engine-lua.enabled"))
                log<<"Trusted Lua source loading requires movement mode in this development build\n";
#else
            if(std::filesystem::is_regular_file(root/"engine-lua.enabled")) log<<"Lua source loading is unavailable in this build\n";
#endif
#ifdef CRML_MOVEMENT_PROBE
            if(conflict) log << "Diagnostic startup refused: diagnostic capture cannot run with gameplay modes\n";
            else if(observe_only) log << observer.start(root) << '\n';
            else if(physics_trial) log << physics.start(root) << '\n';
            log.flush();
#else
            if(observe_only) log << "Engine observer refused: this build does not include diagnostics; mods remain suspended\n";
            else if(physics_trial) log << "Physics trial refused: this build does not include diagnostics; mods remain suspended\n";
            else if(physics_wasm || movement_wasm) log << "Game services unavailable in this build; mods can query capabilities\n";
#endif
            const auto load_started=std::chrono::steady_clock::now();
            if(!diagnostics && !conflict) runtime.load(root / "mods",[&](uint32_t requested) {
#ifdef CRML_MOVEMENT_PROBE
                const bool wants_ui=(requested&(CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|CRML_CAP_UI_PRESENTATION))!=0;
                const bool wants_lists=(requested&CRML_CAP_LISTS)!=0;
                const bool wants_settings=(requested&CRML_CAP_SETTINGS)!=0;
                const bool wants_feedback=(requested&CRML_CAP_FEEDBACK)!=0;
                const bool wants_tutorials=(requested&CRML_CAP_TUTORIALS) && !tutorial_trace;
                if(wants_tutorials) {
                    const bool panel=crml::tutorial_native::start();
                    const bool hint=crml::tutorial::initialize_hint(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
                    log<<"Native tutorials: hint="<<hint<<" panel="<<panel
                       <<" prompt="<<crml::process_tutorials().available(CRML_TUTORIAL_PROMPT)<<'\n';
                }
                if(requested&(CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP)) log<<crml::media::start()<<'\n';
                const bool wants_drawing=(requested&CRML_CAP_DRAWING)!=0;
                if(wants_ui || wants_settings || wants_lists || wants_feedback || wants_tutorials || wants_drawing || std::filesystem::is_regular_file(root/"native-ui.enabled"))
                    log<<crml::native_ui::start(root,wants_ui?&ui:nullptr,wants_settings,wants_feedback,wants_tutorials,wants_drawing,wants_lists)<<'\n';
                if(movement_wasm) requested|=CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION;
                if(physics_wasm) requested|=CRML_CAP_PHYSICS_DAMPING;
                if(std::filesystem::is_regular_file(root/"visibility.enabled")) requested|=CRML_CAP_PLAYER_VISIBILITY;
                constexpr uint32_t player_bits=CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION|CRML_CAP_PLAYER_VISIBILITY|CRML_CAP_PLAYER_NOCLIP|CRML_CAP_PLAYER_READ|CRML_CAP_NAVIGATION_READ;
                if(requested&player_bits) log<<probe.start(root,requested&player_bits)<<'\n';
                else if(std::filesystem::is_regular_file(root/"entity-inspector.enabled") || std::filesystem::is_regular_file(root/"movement-probe.enabled"))
                    log<<probe.start(root)<<'\n';
                if(requested&CRML_CAP_PHYSICS_DAMPING) log<<physics.start(root,true,probe.has_overlay())<<'\n';
                if(requested&CRML_CAP_CAMERA_READ) log<<camera.start()<<'\n';
                if(requested&(CRML_CAP_INPUT_ACTIONS|CRML_CAP_INPUT_BUTTONS)) keyboard.available();
                if(requested&CRML_CAP_ACTION_RULES) session_log.host(crml::action_rules::start()?"Action rule service ready":"Action rule service unavailable for this build");
                log<<"Game service capabilities: "<<services.capabilities()<<'\n';
                if(std::filesystem::is_regular_file(root/"camera-observation.enabled")) log<<observer.start(root,true)<<'\n';
                log.flush();
#endif
            });
            const auto load_finished=std::chrono::steady_clock::now();
#ifdef CRML_LUA_SOURCE
            if(source_requested) log<<(crml::engine::lua::source::active()?
                "Trusted Lua source watcher ready\n":"Lua source loading refused: compatible hooks or source log unavailable\n");
            log.flush();
#endif
            runtime.report_metrics();
            auto last = std::chrono::steady_clock::now();
            const auto metrics_started=last;
            constexpr uint64_t metrics_schedule_ms[]{5000,30000,60000,120000,300000,600000};
            size_t metrics_report{};
            uint64_t loops{},interval_total{},interval_max{},dispatch_total{},dispatch_max{},
                     native_total{},native_max{},gaps_250{},gaps_500{};
            const auto nanoseconds=[](auto duration) {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
            };
            // Attribute worker stalls without per-frame output or engine-thread
            // timers. Fixed storage; reported only with the existing summaries.
            constexpr const char* poll_names[]{"lua","movement","observer","physics","ui","diagnostics","tutorial","context"};
            std::array<uint64_t,std::size(poll_names)> poll_max{};
            auto poll_boundary=load_finished;
            const auto finish_poll=[&](size_t index) {
                const auto end=std::chrono::steady_clock::now();
                poll_max[index]=std::max(poll_max[index],nanoseconds(end-poll_boundary));
                poll_boundary=end;
            };
            const auto report_worker=[&] {
                session_log.host("Worker metrics: load_ns="+std::to_string(nanoseconds(load_finished-load_started))+
                    " loops="+std::to_string(loops)+" interval_total_ns="+std::to_string(interval_total)+
                    " interval_max_ns="+std::to_string(interval_max)+" dispatch_total_ns="+std::to_string(dispatch_total)+
                    " dispatch_max_ns="+std::to_string(dispatch_max)+" native_total_ns="+std::to_string(native_total)+
                    " native_max_ns="+std::to_string(native_max)+" gaps_over_250ms="+std::to_string(gaps_250)+
                    " gaps_over_500ms="+std::to_string(gaps_500)
#ifdef CRML_MOVEMENT_PROBE
                    +" action_rule_skips="+std::to_string(crml::action_restriction_observer::skipped())
                    +" action_rule_warning_adjustments="+std::to_string(crml::action_restriction_observer::warning_adjusted())
#endif
                    );
                std::string stages="Worker poll maxima ns:";
                for(size_t i=0;i<poll_max.size();++i)
                    stages+=" "+std::string(poll_names[i])+"="+std::to_string(poll_max[i]);
                session_log.host(stages);
            };
#ifdef CRML_MOVEMENT_PROBE
            uint64_t media_report_time{},media_report_observations{};
            unsigned media_reports{};
            uint64_t input_context_report_time{};
            unsigned input_context_reports{};
            std::string input_context_last;
            uint64_t menu_context_report_time{};
            unsigned menu_context_reports{};
            std::string menu_context_last;
#endif
            while (runtime.active()
#ifdef CRML_LUA_SOURCE
                   || crml::engine::lua::source::active()
#endif
#ifdef CRML_MOVEMENT_PROBE
                   || probe.active() || observer.active() || physics.active() || crml::native_ui::active() || input_context_active || menu_context_active
#endif
            ) {
#ifdef CRML_MOVEMENT_PROBE
                Sleep(physics.active() || probe.guest_motion() || keyboard.requested()?10:100);
#else
                Sleep(100);
#endif
                const auto now = std::chrono::steady_clock::now();
                const auto interval=nanoseconds(now-last);
                interval_total+=interval;interval_max=std::max(interval_max,interval);
                gaps_250+=interval>250000000;gaps_500+=interval>500000000;
                runtime.tick(std::chrono::duration<float>(now - last).count());
                const auto dispatched=std::chrono::steady_clock::now();
                const auto dispatch=nanoseconds(dispatched-now);
                dispatch_total+=dispatch;dispatch_max=std::max(dispatch_max,dispatch);
                last = now;
                poll_boundary=dispatched;
#ifdef CRML_LUA_SOURCE
                crml::engine::lua::source::poll(GetTickCount64());
#endif
                finish_poll(0);
#ifdef CRML_MOVEMENT_PROBE
                probe.poll();
                finish_poll(1);
                observer.poll();
                finish_poll(2);
                physics.poll();
                finish_poll(3);
                crml::native_ui::poll();
                finish_poll(4);
                if(tutorial_active) crml::tutorial_observer::poll(log);
                if(location_active) crml::navigation_observer::poll(log);
                if(dialogue_active) crml::dialogue_observer::poll(log);
                if(restriction_active) crml::action_restriction_observer::poll(log);
                if(map_active) crml::map_observer::poll(log);
                if(sonar_active) crml::sonar_observer::poll(log);
                finish_poll(5);
                crml::tutorial_native::poll(log);
                finish_poll(6);
                const auto media_now=GetTickCount64();
                if(input_context_active && input_context_reports<64 && media_now-input_context_report_time>=250) {
                    input_context_report_time=media_now;
                    crml::input_context::Snapshot snapshot{};crml::input_context::Read status{};
                    crml::input_context::read(snapshot,status,media_now);
                    const std::string report=std::string("Input context: status=")+crml::input_context::name(status)+
                        " raw="+std::to_string(snapshot.raw_flags)+" derived="+std::to_string(snapshot.derived_flags)+
                        " connected="+std::to_string(snapshot.connected)+" backend="+std::to_string(snapshot.backend)+
                        " generation_matches="+std::to_string(snapshot.generation_matches)+
                        " source_checked="+std::to_string(snapshot.source_checked)+" source="+std::to_string(snapshot.source)+
                        " predicate="+std::to_string(snapshot.predicate_enabled);
                    if(report!=input_context_last) {
                        session_log.host(report+" thread="+std::to_string(snapshot.thread));
                        input_context_last=report;++input_context_reports;
                    }
                }
                if(menu_context_active && menu_context_reports<64 && media_now-menu_context_report_time>=250) {
                    menu_context_report_time=media_now;
                    crml::menu_context::Snapshot snapshot{};crml::menu_context::Read status{};
                    crml::menu_context::read(snapshot,status,media_now);
                    const std::string report=std::string("Menu context: status=")+crml::menu_context::name(status)+
                        " matched="+std::to_string(snapshot.matched_active_context)+
                        " last_match_flag_5c="+std::to_string(snapshot.last_match_flag_5c);
                    // Worker migration is metadata, not a context transition.
                    // Keep the sampled thread in each emitted record without
                    // spending the bounded observation budget on thread churn.
                    if(report!=menu_context_last) {
                        session_log.host(report+" thread="+std::to_string(snapshot.thread));
                        menu_context_last=report;++menu_context_reports;
                    }
                }
                if(media_reports<32 && media_now-media_report_time>=1000 && media.observations()!=media_report_observations) {
                    media_report_time=media_now;media_report_observations=media.observations();++media_reports;
                    crml_media_state snapshot{};media.media_read(snapshot);
                    log<<"Media: observations="<<media_report_observations<<" queued="<<media.submissions()<<" skips="<<media.skips()
                       <<" generation="<<snapshot.generation<<" flags="<<snapshot.flags<<" elapsed_ms="<<snapshot.elapsed_ms<<'\n';
                    log.flush();
                }
#endif
                finish_poll(7);
                const auto finished=std::chrono::steady_clock::now();
                const auto native=nanoseconds(finished-dispatched);
                native_total+=native;native_max=std::max(native_max,native);++loops;
                const auto metrics_age=std::chrono::duration_cast<std::chrono::milliseconds>(finished-metrics_started).count();
                if(metrics_report<std::size(metrics_schedule_ms) && metrics_age>=metrics_schedule_ms[metrics_report]) {
                    runtime.report_metrics();report_worker();
                    // A delayed worker emits one current summary, not a burst
                    // of identical catch-up reports. Bound total session output.
                    do {++metrics_report;} while(metrics_report<std::size(metrics_schedule_ms) && metrics_age>=metrics_schedule_ms[metrics_report]);
                }
            }
#ifdef CRML_MOVEMENT_PROBE
            if(tutorial_active) {crml::tutorial_observer::stop();crml::tutorial_observer::poll(log);}
            if(location_active) {crml::navigation_observer::stop();crml::navigation_observer::poll(log);}
            if(dialogue_active) {crml::dialogue_observer::stop();crml::dialogue_observer::poll(log);}
            if(restriction_active) {crml::action_restriction_observer::stop();crml::action_restriction_observer::poll(log);}
            if(map_active) {crml::map_observer::stop();crml::map_observer::poll(log);}
            if(sonar_active) {crml::sonar_observer::stop();crml::sonar_observer::poll(log);}
#endif
            runtime.shutdown();
            runtime.report_metrics();
            report_worker();
            result = 0;
        } catch (const std::exception& error) {
            OutputDebugStringA(error.what());
        } catch (...) { OutputDebugStringA("CRML runtime initialization failed\n"); }
    });
    return result;
}
