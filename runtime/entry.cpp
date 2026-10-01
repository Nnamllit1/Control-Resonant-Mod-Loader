#include "runtime.h"
#include "compatibility.h"
#ifdef CRML_LUA_SOURCE
#include "lua_source.h"
#endif
#ifdef CRML_MOVEMENT_PROBE
#include "movement_probe.h"
#include "diagnostics/engine_observer.h"
#include "physics_session.h"
#include "input_service.h"
#include "gameplay_router.h"
#include "camera_service.h"
#include "diagnostics/native_ui.h"
#include "ui_service.h"
#include "media_service.h"
#endif
#include <Windows.h>
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
            std::ofstream log(root / "crml.log", std::ios::trunc);
            if (!log) return;
            // Hard session cap prevents guests from growing logs indefinitely.
            size_t written = 0;
#ifdef CRML_MOVEMENT_PROBE
            crml::probe::Recorder probe;
            crml::observer::Recorder observer;
            crml::physics::Session physics;
            crml::KeyboardInput keyboard;
            crml::camera::Service camera;
            auto& ui=crml::ui::process_service();
            auto& media=crml::media::process_service();
            crml::GameplayRouter services(probe,physics,&camera,&ui,&media);
#endif
            crml::Runtime runtime([&](const std::string& text) {
                if (written >= 4 * 1024 * 1024) return;
                const auto line = text.substr(0, 16384);
                log << line << '\n';
                log.flush();
                written += line.size() + 1;
            }
#ifdef CRML_MOVEMENT_PROBE
            , diagnostics || conflict ? nullptr : &services, &keyboard
#endif
            );
            log << "CRML 0.1.0 experimental bootstrap\n";
            if(observe_only) log << "Wasm mods suspended for engine observation\n";
            else if(physics_trial) log << "Wasm mods suspended for native physics trial\n";
            log.flush();
            if(!crml::compatibility::authorize(root,log)) {result=0;return;}
#ifdef CRML_MOVEMENT_PROBE
            struct NativeUiCleanup { ~NativeUiCleanup(){crml::native_ui::stop();crml::media::stop();} } native_ui_cleanup;
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
            if(!diagnostics && !conflict) runtime.load(root / "mods",[&](uint32_t requested) {
#ifdef CRML_MOVEMENT_PROBE
                const bool wants_ui=(requested&(CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|CRML_CAP_UI_PRESENTATION))!=0;
                if(requested&(CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP)) log<<crml::media::start()<<'\n';
                if(wants_ui || std::filesystem::is_regular_file(root/"native-ui.enabled"))
                    log<<crml::native_ui::start(root,wants_ui?&ui:nullptr)<<'\n';
                if(movement_wasm) requested|=CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION;
                if(physics_wasm) requested|=CRML_CAP_PHYSICS_DAMPING;
                if(std::filesystem::is_regular_file(root/"visibility.enabled")) requested|=CRML_CAP_PLAYER_VISIBILITY;
                constexpr uint32_t player_bits=CRML_CAP_PLAYER_MOTION|CRML_CAP_INPUT_MOTION|CRML_CAP_PLAYER_VISIBILITY|CRML_CAP_PLAYER_NOCLIP|CRML_CAP_PLAYER_READ;
                if(requested&player_bits) log<<probe.start(root,requested&player_bits)<<'\n';
                else if(std::filesystem::is_regular_file(root/"entity-inspector.enabled") || std::filesystem::is_regular_file(root/"movement-probe.enabled"))
                    log<<probe.start(root)<<'\n';
                if(requested&CRML_CAP_PHYSICS_DAMPING) log<<physics.start(root,true,probe.has_overlay())<<'\n';
                if(requested&CRML_CAP_CAMERA_READ) log<<camera.start()<<'\n';
                if(requested&(CRML_CAP_INPUT_ACTIONS|CRML_CAP_INPUT_BUTTONS)) keyboard.available();
                log<<"Game service capabilities: "<<services.capabilities()<<'\n';
                if(std::filesystem::is_regular_file(root/"camera-observation.enabled")) log<<observer.start(root,true)<<'\n';
                log.flush();
#endif
            });
#ifdef CRML_LUA_SOURCE
            if(source_requested) log<<(crml::engine::lua::source::active()?
                "Trusted Lua source watcher ready\n":"Lua source loading refused: compatible hooks or source log unavailable\n");
            log.flush();
#endif
            auto last = std::chrono::steady_clock::now();
#ifdef CRML_MOVEMENT_PROBE
            uint64_t media_report_time{},media_report_observations{};
            unsigned media_reports{};
#endif
            while (runtime.active()
#ifdef CRML_LUA_SOURCE
                   || crml::engine::lua::source::active()
#endif
#ifdef CRML_MOVEMENT_PROBE
                   || probe.active() || observer.active() || physics.active() || crml::native_ui::active()
#endif
            ) {
#ifdef CRML_MOVEMENT_PROBE
                Sleep(physics.active() || probe.guest_motion() || keyboard.requested()?10:100);
#else
                Sleep(100);
#endif
                const auto now = std::chrono::steady_clock::now();
                runtime.tick(std::chrono::duration<float>(now - last).count());
                last = now;
#ifdef CRML_LUA_SOURCE
                crml::engine::lua::source::poll(GetTickCount64());
#endif
#ifdef CRML_MOVEMENT_PROBE
                probe.poll();
                observer.poll();
                physics.poll();
                crml::native_ui::poll();
                const auto media_now=GetTickCount64();
                if(media_reports<32 && media_now-media_report_time>=1000 && media.observations()!=media_report_observations) {
                    media_report_time=media_now;media_report_observations=media.observations();++media_reports;
                    crml_media_state snapshot{};media.media_read(snapshot);
                    log<<"Media: observations="<<media_report_observations<<" queued="<<media.submissions()<<" skips="<<media.skips()
                       <<" generation="<<snapshot.generation<<" flags="<<snapshot.flags<<" elapsed_ms="<<snapshot.elapsed_ms<<'\n';
                    log.flush();
                }
#endif
            }
            runtime.shutdown();
            result = 0;
        } catch (const std::exception& error) {
            OutputDebugStringA(error.what());
        } catch (...) { OutputDebugStringA("CRML runtime initialization failed\n"); }
    });
    return result;
}
