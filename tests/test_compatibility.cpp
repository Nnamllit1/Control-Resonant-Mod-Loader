#include "compatibility.h"
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace crml::compatibility;
namespace crml::compatibility { Choice test_prompt(bool&,PFTASKDIALOGCALLBACK,LONG_PTR); }
HRESULT CALLBACK close_prompt(HWND window,UINT message,WPARAM,LPARAM,LONG_PTR data) {
    if(message==TDN_CREATED) {
        auto* owned=reinterpret_cast<bool*>(data);
        DWORD process{};GetWindowThreadProcessId(window,&process);
        *owned=process==GetCurrentProcessId() && GetParent(window)==nullptr;
        // Exercise our dialog directly; no keyboard injection or game process.
        SendMessageW(window,TDM_CLICK_VERIFICATION,TRUE,FALSE);
        SendMessageW(window,TDM_CLICK_BUTTON,IDNO,0);
    }
    return S_OK;
}
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
int main() {
    try {
        const auto root=std::filesystem::absolute("compatibility-test-"+std::to_string(GetCurrentProcessId()));
        require(std::filesystem::create_directory(root),"exclusive fixture directory");
        struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);}} cleanup{root};
        const auto path=root/"compatibility-choice.txt";
        const std::string first(64,'a'),second(64,'b');
        require(allowed(tested_sha) && !allowed(first) && !allowed(""),"unknown executable requires approval");
        require(read_choice(path,first)==Choice::ask,"missing choice asks");
        require(write_choice(path,first,Choice::allow) && read_choice(path,first)==Choice::allow,"remember continue");
        require(read_choice(path,second)==Choice::ask,"executable update requires new decision");
        require(write_choice(path,first,Choice::deny) && read_choice(path,first)==Choice::deny,"remember decline");
        require(!write_choice(path,"invalid",Choice::allow),"invalid digest not persisted");
        {std::ofstream out(path);out<<first<<"\nallow\nextra";}
        require(read_choice(path,first)==Choice::ask,"corrupt choice cannot authorize");
        {std::ofstream out(path);out<<std::string(1000,'a');}
        require(read_choice(path,first)==Choice::ask,"oversized file rejected");
        std::memcpy(approved_sha,first.c_str(),65);
        require(allowed(first) && !allowed(second),"approval scoped to one executable");
        unsigned char a[]{0x40,0x53},b[]{0x40,0x54};
        require(matches(a,a,2) && !matches(a,b,2),"hook signatures still required");
        require(!matches(reinterpret_cast<void*>(1),a,2),"unmapped target rejected without crash");
        auto page=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);
        require(page && !matches(page,a,2),"unreadable target rejected without crash");
        VirtualFree(page,0,MEM_RELEASE);
        bool remember{},owned{};
        require(test_prompt(remember,&close_prompt,reinterpret_cast<LONG_PTR>(&owned))==Choice::deny && remember && owned,
                "loader-owned native prompt and remember/decline buttons work without game UI");
        std::cout<<"Compatibility decisions, executable binding and guarded signatures passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
