#include "runtime.h"
#ifdef CRML_MOVEMENT_PROBE
#include "movement_probe.h"
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
            std::ofstream log(root / "crml.log", std::ios::trunc);
            if (!log) return;
            // Hard session cap prevents guests from growing logs indefinitely.
            size_t written = 0;
#ifdef CRML_MOVEMENT_PROBE
            crml::probe::Recorder probe;
#endif
            crml::Runtime runtime([&](const std::string& text) {
                if (written >= 4 * 1024 * 1024) return;
                const auto line = text.substr(0, 16384);
                log << line << '\n';
                log.flush();
                written += line.size() + 1;
            }
#ifdef CRML_MOVEMENT_PROBE
            , &probe
#endif
            );
            log << "CRML 0.1.0 experimental bootstrap\n";
            log.flush();
#ifdef CRML_MOVEMENT_PROBE
            log << probe.start(root) << '\n';
            log.flush();
#endif
            runtime.load(root / "mods");
            auto last = std::chrono::steady_clock::now();
            while (runtime.active()
#ifdef CRML_MOVEMENT_PROBE
                   || probe.active()
#endif
            ) {
                Sleep(100);
                const auto now = std::chrono::steady_clock::now();
                runtime.tick(std::chrono::duration<float>(now - last).count());
                last = now;
#ifdef CRML_MOVEMENT_PROBE
                probe.poll();
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
