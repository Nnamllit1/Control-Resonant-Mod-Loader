#include "compatibility.h"
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace crml::compatibility;
namespace crml::compatibility {
Choice test_prompt(bool&,PFTASKDIALOGCALLBACK,LONG_PTR);
HRESULT test_prompt_link(HWND,const wchar_t*,bool(*)(HWND));
}
unsigned link_opens{};
bool failed_link{};
bool record_link(HWND window) {++link_opens;return IsWindow(window) && !failed_link;}
HRESULT CALLBACK close_prompt(HWND window,UINT message,WPARAM,LPARAM,LONG_PTR data) {
    if(message==TDN_CREATED) {
        auto* owned=reinterpret_cast<bool*>(data);
        DWORD process{};GetWindowThreadProcessId(window,&process);
        *owned=process==GetCurrentProcessId() && GetParent(window)==nullptr;
        test_prompt_link(window,L"https://example.invalid/",record_link);
        test_prompt_link(window,nullptr,record_link);
        *owned=*owned && link_opens==0;
        test_prompt_link(window,releases_url,record_link);
        *owned=*owned && link_opens==1 && IsWindow(window);
        failed_link=true;
        test_prompt_link(window,releases_url,record_link);
        *owned=*owned && link_opens==2 && IsWindow(window);
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
        require(allowed(updated_sha) && known_build(updated_sha),"October executable has its own profile");
        select_profile(tested_sha);
        require(address(0x10000000,0x1b98950)==0x11b98950,"previous movement address retained");
        const auto previous_document=ui_document_sha();
        select_profile(updated_sha);
        require(address(0x10000000,0x1b98950)==0x11bbf6e0,"updated movement address selected");
        require(address(0x10000000,0x5c00ca4)==0x15cd1424,"player tag data relocated");
        require(!address(0x10000000,0x123456) && !address(0,0x1b98950),"unmapped address refuses access");
        require(ui_document_sha()!=previous_document,"UI document fingerprint follows executable");
        uintptr_t last{};
        for(const auto& entry:october_addresses) {
            require(entry.previous>last && entry.current,"profile addresses unique and sorted");last=entry.previous;
            if(entry.previous>=0x4000000) continue;
            auto bytes=entry.prefix;
            require(matches_code(bytes.data(),entry.previous,nullptr,bytes.size()),"updated exact signature accepted");
            bytes[0]^=1;
            require(!matches_code(bytes.data(),entry.previous,nullptr,bytes.size()),"changed updated entry refused");
        }
        require(!matches_code(nullptr,0x123456,nullptr,1),"missing updated signature refuses access");
        select_profile(first);
        require(!known_build(first) && address(0x10000000,0x1b98950)==0x11b98950,
                "unknown consent cannot inherit updated profile");
        require(ui_document_sha()==previous_document,"profile reset restores previous UI fingerprint");
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
