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
decltype(&PeekMessageW) peek_w{};
decltype(&PeekMessageA) peek_a{};
decltype(&GetMessageW) message_w{};
decltype(&GetMessageA) message_a{};
decltype(&GetRawInputData) raw_data{};
decltype(&GetKeyboardState) keyboard_state{};
decltype(&GetKeyState) key_state{};

bool capture(void* caller) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(caller);
    if(!ready.load(std::memory_order_acquire) || address<executable || address>=executable_end) return false;
    const DWORD error=GetLastError();
    const bool result=active_callback();
    SetLastError(error);
    return result;
}
BOOL WINAPI peekW(LPMSG out,HWND window,UINT first,UINT last,UINT remove) {
    const auto result=peek_w(out,window,first,last,remove);
    if(result && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI peekA(LPMSG out,HWND window,UINT first,UINT last,UINT remove) {
    const auto result=peek_a(out,window,first,last,remove);
    if(result && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI messageW(LPMSG out,HWND window,UINT first,UINT last) {
    const auto result=message_w(out,window,first,last);
    if(result>0 && capture(_ReturnAddress()) && filter(*out)) ++filtered;
    return result;
}
BOOL WINAPI messageA(LPMSG out,HWND window,UINT first,UINT last) {
    const auto result=message_a(out,window,first,last);
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
    if(!callback) return false;
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
    // Release cached legacy actions even when a key was held before F6. Physical
    // state is untouched: the noclip bridge continues polling GetAsyncKeyState.
    for(auto key:keys) {
        if(!key || !(GetAsyncKeyState(key)&0x8000)) continue;
        const auto scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC_EX);
        const LPARAM flags=LPARAM{0xc0000001} | LPARAM((scan&0xff)<<16) | (scan&0xff00 ? LPARAM{1}<<24 : 0);
        PostMessageW(window,WM_KEYUP,key,flags);
    }
}
uint64_t consumed() noexcept { return filtered.load(std::memory_order_relaxed); }
}
