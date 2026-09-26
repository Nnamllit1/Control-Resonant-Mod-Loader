#include "runtime.h"
#ifdef CRML_MOVEMENT_PROBE
#include "movement_probe.h"
#include "engine_observer.h"
#include "physics_session.h"
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
            const bool diagnostics=observe_only || physics_trial;
            std::ofstream log(root / "crml.log", std::ios::trunc);
            if (!log) return;
            // Hard session cap prevents guests from growing logs indefinitely.
            size_t written = 0;
#ifdef CRML_MOVEMENT_PROBE
            crml::probe::Recorder probe;
            crml::observer::Recorder observer;
            crml::physics::Session physics;
#endif
            crml::Runtime runtime([&](const std::string& text) {
                if (written >= 4 * 1024 * 1024) return;
                const auto line = text.substr(0, 16384);
                log << line << '\n';
                log.flush();
                written += line.size() + 1;
            }
#ifdef CRML_MOVEMENT_PROBE
            , diagnostics ? nullptr : &probe
#endif
            );
            log << "CRML 0.1.0 experimental bootstrap\n";
            if(observe_only) log << "Wasm mods suspended for engine observation\n";
            else if(physics_trial) log << "Wasm mods suspended for native physics trial\n";
            log.flush();
#ifdef CRML_MOVEMENT_PROBE
            if(observe_only && physics_trial) log << "Diagnostic startup refused: conflicting observer and physics trial markers\n";
            else log << (observe_only ? observer.start(root) : physics_trial ? physics.start(root) : probe.start(root)) << '\n';
            log.flush();
#else
            if(observe_only) log << "Engine observer refused: this build does not include diagnostics; mods remain suspended\n";
            else if(physics_trial) log << "Physics trial refused: this build does not include diagnostics; mods remain suspended\n";
#endif
            if(!diagnostics) runtime.load(root / "mods");
            auto last = std::chrono::steady_clock::now();
            while (runtime.active()
#ifdef CRML_MOVEMENT_PROBE
                   || probe.active() || observer.active() || physics.active()
#endif
            ) {
                Sleep(100);
                const auto now = std::chrono::steady_clock::now();
                runtime.tick(std::chrono::duration<float>(now - last).count());
                last = now;
#ifdef CRML_MOVEMENT_PROBE
                probe.poll();
                observer.poll();
                physics.poll();
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
