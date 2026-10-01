#include "input_filter.h"
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstddef>

namespace crml::probe::input {
namespace {
constexpr std::array<unsigned,11> keys{'W','A','S','D',VK_SPACE,VK_CONTROL,VK_LCONTROL,VK_RCONTROL,VK_SHIFT,VK_LSHIFT,VK_RSHIFT};
Active active_callback{};
uintptr_t executable{}, executable_end{};
std::atomic<bool> ready{};
std::atomic<uint64_t> filtered{};
std::array<std::atomic<BYTE>,256> cached{};
std::atomic<uint64_t> cached_tick{};
decltype(&PeekMessageW) peek_w{};
decltype(&PeekMessageA) peek_a{};
decltype(&GetMessageW) message_w{};
decltype(&GetMessageA) message_a{};
decltype(&GetRawInputData) raw_data{};
decltype(&GetKeyboardState) keyboard_state{};
decltype(&GetKeyState) key_state{};

bool game_caller(void* caller) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(caller);
    return ready.load(std::memory_order_acquire) && address>=executable && address<executable_end;
}
void publish(const BYTE* state,uint64_t tick) noexcept {
    for(size_t i=0;i<cached.size();++i) cached[i].store(state[i],std::memory_order_relaxed);
    cached_tick.store(tick,std::memory_order_release);
}
void observe(void* caller) noexcept {
    // Observation may pass through another mod's hook first. Thread/window
    // ownership scopes this read; only suppression requires an executable caller.
    (void)caller;
    if(!ready.load(std::memory_order_acquire)) return;
    const auto error=GetLastError();DWORD process{};
    const auto thread=GetWindowThreadProcessId(GetForegroundWindow(),&process);
    if(process==GetCurrentProcessId() && thread==GetCurrentThreadId()) {
        BYTE state[256]{};
        if(keyboard_state(state)) publish(state,GetTickCount64());
    }
    SetLastError(error);
}

bool capture(void* caller) noexcept {
    if(!active_callback || !game_caller(caller)) return false;
    const DWORD error=GetLastError();
    const bool result=active_callback();
    SetLastError(error);
    return result;
}
BOOL WINAPI peekW(LPMSG out,HWND window,UINT first,UINT last,UINT remove) {
    const auto result=peek_w(out,window,first,last,remove);
    observe(_ReturnAddress());
    if(result && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI peekA(LPMSG out,HWND window,UINT first,UINT last,UINT remove) {
    const auto result=peek_a(out,window,first,last,remove);
    observe(_ReturnAddress());
    if(result && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI messageW(LPMSG out,HWND window,UINT first,UINT last) {
    const auto result=message_w(out,window,first,last);
    observe(_ReturnAddress());
    if(result>0 && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI messageA(LPMSG out,HWND window,UINT first,UINT last) {
    const auto result=message_a(out,window,first,last);
    observe(_ReturnAddress());
    if(result>0 && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
UINT WINAPI rawData(HRAWINPUT handle,UINT command,LPVOID data,PUINT size,UINT header_size) {
    const auto result=raw_data(handle,command,data,size,header_size);
    if(result!=UINT(-1) && command==RID_INPUT && data && header_size==sizeof(RAWINPUTHEADER) &&
       result>=sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD) && capture(_ReturnAddress()) &&
       filter(*static_cast<RAWINPUT*>(data),result)) ++filtered;
    return result;
}
BOOL WINAPI keyboardState(PBYTE state) {
    const auto result=keyboard_state(state);
    if(result && capture(_ReturnAddress())) filter(state);
    return result;
}
SHORT WINAPI keyState(int key) {
    const auto result=key_state(key);
    return owned(key) && capture(_ReturnAddress()) ? SHORT(result&1) : result;
}
}
bool owned(unsigned key) noexcept {
    if(!key) return false;
    for(auto candidate:keys) if(key==candidate) return true;
    return false;
}
bool down(unsigned key) noexcept {
    if(key>=cached.size()) return false;
    const auto tick=cached_tick.load(std::memory_order_acquire),now=GetTickCount64();
    return tick && now>=tick && now-tick<=500 && (cached[key].load(std::memory_order_relaxed)&0x80)!=0;
}
bool filter(MSG& message) noexcept {
    if((message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN) && owned(static_cast<unsigned>(message.wParam))) {
        message.message=message.message==WM_KEYDOWN ? WM_KEYUP : WM_SYSKEYUP;
        message.lParam|=LPARAM{0xc0000000};
        return true;
    }
    if(message.message==WM_CHAR || message.message==WM_SYSCHAR) {
        const auto key=static_cast<unsigned>(message.wParam);
        if(key==' ' || key=='w' || key=='a' || key=='s' || key=='d' ||
           key=='W' || key=='A' || key=='S' || key=='D' || key==1 || key==4 || key==19 || key==23) {
            message.message=WM_NULL; message.wParam=0; message.lParam=0;
            return true;
        }
    }
    return false;
}
bool filter(RAWINPUT& event,UINT bytes) noexcept {
    if(bytes<offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD) || event.header.dwType!=RIM_TYPEKEYBOARD ||
       event.header.dwSize<offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD) || event.header.dwSize>bytes) return false;
    auto& key=event.data.keyboard;
    if(!owned(key.VKey) || (key.Flags&RI_KEY_BREAK)) return false;
    key.Flags|=RI_KEY_BREAK;
    key.Message=key.Message==WM_SYSKEYDOWN ? WM_SYSKEYUP : WM_KEYUP;
    return true;
}
void filter(BYTE* state) noexcept { for(auto key:keys) if(key) state[key]&=1; }

bool start(Active callback) noexcept {
    // Observation can share an existing suppression owner without replacing it.
    if(ready.load(std::memory_order_acquire)) return !callback || active_callback==callback;
    executable=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(executable);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(executable+dos->e_lfanew);
    executable_end=executable+nt->OptionalHeader.SizeOfImage;
    active_callback=callback;
    struct Hook { const char* name; void* detour; void** original; void* target{}; };
    Hook hooks[]{
        {"PeekMessageW",reinterpret_cast<void*>(&peekW),reinterpret_cast<void**>(&peek_w)},
        {"PeekMessageA",reinterpret_cast<void*>(&peekA),reinterpret_cast<void**>(&peek_a)},
        {"GetMessageW",reinterpret_cast<void*>(&messageW),reinterpret_cast<void**>(&message_w)},
        {"GetMessageA",reinterpret_cast<void*>(&messageA),reinterpret_cast<void**>(&message_a)},
        {"GetRawInputData",reinterpret_cast<void*>(&rawData),reinterpret_cast<void**>(&raw_data)},
        {"GetKeyboardState",reinterpret_cast<void*>(&keyboardState),reinterpret_cast<void**>(&keyboard_state)},
        {"GetKeyState",reinterpret_cast<void*>(&keyState),reinterpret_cast<void**>(&key_state)}
    };
    for(auto& hook:hooks) {
        hook.target=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),hook.name));
        if(!hook.target || MH_CreateHook(hook.target,hook.detour,hook.original)!=MH_OK) return false;
    }
    // On partial failure all installed hooks remain pass-through for process lifetime.
    for(auto& hook:hooks) if(MH_EnableHook(hook.target)!=MH_OK) return false;
    ready.store(true,std::memory_order_release);
    return true;
}
void release_held(HWND window) noexcept {
    DWORD process{};
    if(!window || !GetWindowThreadProcessId(window,&process) || process!=GetCurrentProcessId()) return;
    // Release cached legacy actions even when a key was held before F6.
    // CRML keeps reading its non-consuming snapshot of the real queue state.
    for(auto key:keys) {
        if(!key || !down(key)) continue;
        const auto scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC_EX);
        const LPARAM flags=LPARAM{0xc0000001} | LPARAM((scan&0xff)<<16) | (scan&0xff00 ? LPARAM{1}<<24 : 0);
        PostMessageW(window,WM_KEYUP,key,flags);
    }
}
uint64_t consumed() noexcept { return filtered.load(std::memory_order_relaxed); }
bool observing() noexcept { return ready.load(std::memory_order_acquire); }
#ifdef CRML_INPUT_TESTING
namespace testing { void publish(const BYTE* state,uint64_t tick) noexcept { input::publish(state,tick); } }
#endif
}
