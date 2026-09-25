#include "input_filter.h"
#include <MinHook.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace input=crml::probe::input;
namespace {
bool enabled{};
bool active() noexcept { return enabled; }
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void post(UINT message,WPARAM key,LPARAM flags=1) {
    require(PostThreadMessageW(GetCurrentThreadId(),message,key,flags)!=0,"Message enqueue failed");
}
}
int main() {
    try {
        for(unsigned key:{unsigned('W'),unsigned('A'),unsigned('S'),unsigned('D'),unsigned(VK_SPACE),unsigned(VK_CONTROL),unsigned(VK_LCONTROL),unsigned(VK_RCONTROL),
                          unsigned(VK_SHIFT),unsigned(VK_LSHIFT),unsigned(VK_RSHIFT)}) {
            MSG message{}; message.message=WM_KEYDOWN; message.wParam=key; message.lParam=0x00110001;
            require(input::filter(message) && message.message==WM_KEYUP && message.lParam==LPARAM{0xc0110001},"Owned key was not released");
            require(!input::filter(message),"Key-up was consumed");
            RAWINPUT raw{}; raw.header.dwSize=sizeof(raw); raw.header.dwType=RIM_TYPEKEYBOARD;
            raw.data.keyboard.VKey=static_cast<USHORT>(key); raw.data.keyboard.Flags=RI_KEY_E0; raw.data.keyboard.MakeCode=0x1d;
            raw.data.keyboard.Message=WM_SYSKEYDOWN;
            require(input::filter(raw,sizeof(raw)) && raw.data.keyboard.Flags==(RI_KEY_E0|RI_KEY_BREAK) &&
                    raw.data.keyboard.MakeCode==0x1d && raw.data.keyboard.Message==WM_SYSKEYUP,"Raw release lost scan-code flags");
            require(!input::filter(raw,sizeof(raw)),"Raw key-up changed");
        }
        for(unsigned key:{unsigned(VK_ESCAPE),unsigned(VK_F6),unsigned(VK_MENU),unsigned(VK_TAB),unsigned('E')}) {
            MSG message{}; message.message=WM_KEYDOWN; message.wParam=key;
            require(!input::filter(message) && message.message==WM_KEYDOWN,"Unowned key consumed");
        }
        RAWINPUT mouse{}; mouse.header.dwSize=sizeof(mouse); mouse.header.dwType=RIM_TYPEMOUSE;
        mouse.data.mouse.lLastX=42; const auto before=mouse;
        require(!input::filter(mouse,sizeof(mouse)) && !std::memcmp(&mouse,&before,sizeof(mouse)),"Mouse look modified");
        RAWINPUT short_event{}; short_event.header.dwType=RIM_TYPEKEYBOARD; short_event.header.dwSize=sizeof(short_event);
        short_event.data.keyboard.VKey=VK_SPACE;
        require(!input::filter(short_event,sizeof(RAWINPUTHEADER)),"Short raw buffer accepted");
        std::array<BYTE,256> state; state.fill(0x81); input::filter(state.data());
        for(unsigned i=0;i<state.size();++i) require(state[i]==(input::owned(i)?1:0x81),"Keyboard state filtering changed another key or toggle bit");
        MSG text{}; text.message=WM_CHAR; text.wParam=' ';
        require(input::filter(text) && text.message==WM_NULL,"Space text leaked");
        text.message=WM_CHAR; text.wParam='x'; require(!input::filter(text),"Other text consumed");

        // Exercise real User32 trampolines with a thread-local queue. No global
        // input is injected and no keyboard state outside this process is changed.
        MSG message{}; PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
        require(MH_Initialize()==MH_OK && input::start(&active),"Input hooks failed to install");
        enabled=true;
        post(WM_KEYDOWN,VK_SPACE,0x00390001);
        require(PeekMessageW(&message,HWND(-1),0,0,PM_NOREMOVE) && message.message==WM_KEYUP,"Peek W failed to consume Space");
        enabled=false;
        require(PeekMessageW(&message,HWND(-1),0,0,PM_REMOVE) && message.message==WM_KEYDOWN,"NOREMOVE mutated the actual message queue");
        enabled=true;
        post(WM_KEYDOWN,'W');
        require(PeekMessageA(&message,HWND(-1),0,0,PM_REMOVE) && message.message==WM_KEYUP,"Peek A failed to consume W");
        post(WM_KEYDOWN,'A'); require(GetMessageW(&message,HWND(-1),0,0)>0 && message.message==WM_KEYUP,"GetMessage W failed");
        post(WM_KEYDOWN,'D'); require(GetMessageA(&message,HWND(-1),0,0)>0 && message.message==WM_KEYUP,"GetMessage A failed");
        post(WM_KEYDOWN,VK_ESCAPE); require(PeekMessageW(&message,HWND(-1),0,0,PM_REMOVE) && message.message==WM_KEYDOWN,"Escape was blocked");
        enabled=false;
        post(WM_KEYDOWN,VK_SPACE); require(GetMessageW(&message,HWND(-1),0,0)>0 && message.message==WM_KEYDOWN,"Normal controls did not return");
        enabled=true;
        post(WM_QUIT,17); require(GetMessageW(&message,HWND(-1),0,0)==0 && message.wParam==17,"Quit status changed");
        require(input::consumed()>=4,"Input diagnostics were not updated");
        require(MH_Uninitialize()==MH_OK,"Input hook cleanup failed");
        std::cout << "Raw/legacy keyboard filtering, real User32 hooks, mouse/menu passthrough, and restoration passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
