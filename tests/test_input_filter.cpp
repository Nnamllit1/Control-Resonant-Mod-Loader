#include "input_filter.h"
#include "noclip.h"
#include <algorithm>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace input=crml::probe::input;
namespace {
bool enabled{};
bool active() noexcept { return enabled; }
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void post(UINT message,WPARAM key,LPARAM flags=1) {
    require(PostThreadMessageW(GetCurrentThreadId(),message,key,flags)!=0,"Message enqueue failed");
}
}
int main(int argc,char** argv) {
    try {
        std::array<BYTE,256> snapshot{};
        snapshot[VK_INSERT]=0x80;snapshot[VK_F6]=0x80;snapshot['W']=0x80;
        snapshot[VK_CAPITAL]=1;
        input::testing::publish(snapshot.data(),GetTickCount64());
        for(unsigned i=0;i<1000;++i) require(input::down(VK_INSERT),"Reading our cached key must not consume another reader's edge");
        require(input::down(VK_F6) && input::down('W') && !input::down(VK_CAPITAL) && !input::down(256),"Cached high-bit and bounds handling");
        input::testing::publish(snapshot.data(),GetTickCount64()-501);
        require(!input::down(VK_INSERT) && !input::fresh(),"Stale keyboard cache is not usable input");
        snapshot.fill(0);input::testing::publish(snapshot.data(),GetTickCount64());
        require(!input::down('W') && input::fresh(),"Released keys still constitute a fresh input sample");
        {
            std::array<BYTE,256> copy{};
            snapshot.fill(0);snapshot[VK_F6]=snapshot[VK_ESCAPE]=0x80;
            input::testing::publish(snapshot.data(),1000);
            require(input::snapshot(copy.data(),1500) && copy[VK_F6]==0x80 && copy[VK_ESCAPE]==0x80,
                    "Context keys must be copied from the same publication");
            require(!input::snapshot(copy.data(),1501) && std::all_of(copy.begin(),copy.end(),[](BYTE b){return b==0;}),
                    "Stale copied keyboard must be zeroed");
            require(!input::snapshot(copy.data(),999),"Backwards snapshot clock must fail closed");
            std::atomic<bool> stop{};
            std::atomic<unsigned> publications{};
            std::thread publisher([&] {
                std::array<BYTE,256> pressed{},released{};pressed.fill(0x80);
                while(!stop.load()) {
                    input::testing::publish(pressed.data(),1000);
                    input::testing::publish(released.data(),1000);
                    ++publications;
                    std::this_thread::yield();
                }
            });
            while(!publications.load()) std::this_thread::yield();
            bool consistent=true;
            unsigned copies{};
            for(unsigned n=0;n<5000;++n) {
                if(input::snapshot(copy.data(),1001)) {
                    ++copies;
                    // Initial publication and either concurrent publication
                    // agree on these keys; a mixed copy can invent release.
                    if(copy[VK_F6]!=copy[VK_ESCAPE]) consistent=false;
                } else if(std::any_of(copy.begin(),copy.end(),[](BYTE b){return b!=0;})) consistent=false;
                std::this_thread::yield();
            }
            stop=true;publisher.join();
            require(consistent,"Concurrent keyboard snapshot must not mix publication or failure bytes");
            require(copies>0,"Snapshot stress test must include successful copies after publisher startup");
            snapshot.fill(0);input::testing::publish(snapshot.data(),GetTickCount64());
        }
        {
            using crml::probe::Flight;
            using crml::probe::Sample;
            using crml::probe::StopReason;
            input::testing::publish(snapshot.data(),0);
            require(!input::testing::fresh_at(1000),"Never-observed keyboard must not authorize flight");
            input::testing::publish(snapshot.data(),1000);
            require(!input::testing::fresh_at(999) && input::testing::fresh_at(1500) &&
                    !input::testing::fresh_at(1501),"Input age and backwards clock boundaries");
            Sample sample{};sample.entity=42;sample.world=100;
            Flight flight;std::array<float,3> target{};
            // Worker and controller remain fresh while keyboard publication
            // stops. Renewing guest velocity must not bypass keyboard expiry.
            for(uint64_t now=1000;now<=1500;now+=100) {
                require(flight.request_motion(1,true,5,0,0,sample,now,input::testing::fresh_at(now))==1,
                        "Fresh keyboard and controller must accept guest motion");
                require(flight.step(sample,sample.world,now,true,input::testing::fresh_at(now),flight.requested,target),
                        "Fresh controller path must run before keyboard expiry");
                std::copy(target.begin(),target.end(),sample.position);
            }
            const auto before=target;
            require(!flight.step(sample,sample.world,1501,true,input::testing::fresh_at(1501),flight.requested,target) &&
                    !flight.enabled && !flight.owner && target==before && flight.stopped.reason==StopReason::stale_sample,
                    "Controller callback must cancel before applying stale-keyboard velocity");
            // Also reject renewal without waiting for a controller callback.
            flight=Flight{};
            require(flight.request_motion(1,true,0,0,0,sample,1500,true)==1,"Arm hover lease");
            require(flight.request_motion(1,true,0,0,0,sample,1501,input::testing::fresh_at(1501))==-1 &&
                    !flight.enabled && flight.stopped.reason==StopReason::stale_sample,
                    "Stale keyboard must reject hover renewal with a fresh controller");
            require(flight.request_motion(1,false,0,0,0,sample,1502,false)==0,
                    "Explicit cleanup must remain available with stale input");
            require(flight.request_motion(1,true,0,0,0,sample,1503,false)==-1 && !flight.enabled,
                    "Stale keyboard must reject first activation");
            input::testing::publish(snapshot.data(),GetTickCount64());
        }
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
        for(unsigned key=VK_F1;key<=VK_F24;++key) {
            MSG message{};message.message=WM_KEYDOWN;message.wParam=key;
            require(!input::filter(message) && message.message==WM_KEYDOWN,"Third-party function hotkey consumed");
        }
        for(unsigned key:{unsigned(VK_INSERT),unsigned(VK_HOME),unsigned(VK_DELETE),unsigned(VK_END)}) {
            MSG message{};message.message=WM_KEYDOWN;message.wParam=key;
            require(!input::filter(message) && message.message==WM_KEYDOWN,"Third-party menu hotkey consumed");
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
        require(MH_Initialize()==MH_OK,"MinHook initialization failed");
        if(argc==2 && std::strcmp(argv[1],"--observer-first")==0) {
            require(input::start() && input::observing(),"Observer-first installation failed");
            enabled=true;
            post(WM_KEYDOWN,VK_SPACE);
            require(PeekMessageW(&message,HWND(-1),0,0,PM_REMOVE) && message.message==WM_KEYDOWN,
                    "An observer without a suppression owner must not consume keys");
            enabled=false;
        }
        require(input::start(&active),"Input hooks failed to acquire suppression owner");
        require(input::start(&active),"Same suppression owner must be idempotent");
        std::array<bool,8> shared{};
        std::array<std::thread,8> callers;
        for(size_t i=0;i<callers.size();++i) callers[i]=std::thread([&,i] {shared[i]=input::start(&active);});
        for(auto& caller:callers) caller.join();
        for(bool result:shared) require(result,"Concurrent starts must retain the same suppression owner");
        require(input::start() && input::observing(),"Read-only observer must share the suppression hook");
        require(!input::start(+[]() noexcept {return false;}),"Another suppression owner must be rejected");
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
