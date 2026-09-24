#include <Windows.h>

namespace {
INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE runtime_once = INIT_ONCE_STATIC_INIT;
FARPROC functions[14]{};
constexpr WORD ordinals[]{2, 3, 4, 5, 7, 8, 10, 100, 101, 102, 103, 104, 108, 109};
DWORD WINAPI unavailable() { return ERROR_PROC_NOT_FOUND; }

BOOL CALLBACK load_system(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(path, MAX_PATH);
    if (length && length < MAX_PATH - 15 && wcscat_s(path, L"\\xinput1_4.dll") == 0) {
        const auto library = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) for (size_t i = 0; i < 14; ++i) functions[i] = GetProcAddress(library, MAKEINTRESOURCEA(ordinals[i]));
    }
    for (auto& function : functions) if (!function) function = reinterpret_cast<FARPROC>(unavailable);
    return TRUE;
}

DWORD WINAPI start_runtime(void* argument) {
    const auto self = static_cast<HMODULE>(argument);
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(self, path, 32768);
    if (!count || count >= 32768) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    slash[1] = 0;
    if (wcscat_s(path, L"crml\\crml_runtime.dll") != 0) return 1;
    // Trusted runtime and Wasmtime are adjacent; no dependency search through CWD.
    const auto runtime = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) { OutputDebugStringW(L"CRML: runtime could not be loaded\n"); return 1; }
    using Run = DWORD (WINAPI*)();
    const auto run = reinterpret_cast<Run>(GetProcAddress(runtime, "crml_run"));
    if (!run) return 1;
    // Both libraries remain resident for the lifetime of the process.
    return run();
}
BOOL CALLBACK begin_runtime(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&begin_runtime), &self)) {
        const auto thread = CreateThread(nullptr, 0, start_runtime, self, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
}

extern "C" FARPROC crml_resolve(unsigned index) {
    InitOnceExecuteOnce(&system_once, load_system, nullptr, nullptr);
    // The observed game import is ordinal 2. Bootstrap on its first invocation,
    // never during this DLL's process-attach callback.
    if (index == 0) InitOnceExecuteOnce(&runtime_once, begin_runtime, nullptr, nullptr);
    return functions[index];
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
