#include "compatibility.h"
#include <commctrl.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <array>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace crml::compatibility {
namespace {
bool valid_sha(std::string_view sha) {
    return sha.size()==64 && sha.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}
std::string hash_file(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(!file || BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) {BCryptCloseAlgorithmProvider(algorithm,0);return {};}
    std::array<unsigned char,65536> block{};bool ok=true;
    while(file) {
        file.read(reinterpret_cast<char*>(block.data()),block.size());
        if(file.gcount() && BCryptHashData(hash,block.data(),static_cast<ULONG>(file.gcount()),0)<0) {ok=false;break;}
    }
    std::array<unsigned char,32> digest{};
    ok=ok && file.eof() && BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok) return {};
    std::ostringstream out;
    for(auto b:digest) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);
    return out.str();
}
using OpenReleases=bool(*)(HWND);
bool open_releases(HWND window) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",releases_url,nullptr,nullptr,SW_SHOWNORMAL))>32;
}
struct PromptContext {PFTASKDIALOGCALLBACK observer{};LONG_PTR data{};OpenReleases open{open_releases};};
HRESULT CALLBACK prompt_event(HWND window,UINT message,WPARAM wparam,LPARAM lparam,LONG_PTR data) {
    auto& context=*reinterpret_cast<PromptContext*>(data);
    if(message==TDN_HYPERLINK_CLICKED) {
        // Only the authored release link is actionable. Opening it neither
        // dismisses this dialog nor changes the stored compatibility decision.
        if(lparam && std::wcscmp(reinterpret_cast<const wchar_t*>(lparam),releases_url)==0 && !context.open(window))
            SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_FOOTER,reinterpret_cast<LPARAM>(
                L"Could not open your browser. Visit https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases to check for an update."));
        return S_OK;
    }
    return context.observer?context.observer(window,message,wparam,lparam,context.data):S_OK;
}
Choice prompt(bool& remember,PFTASKDIALOGCALLBACK callback=nullptr,LONG_PTR callback_data=0) {
    // Activate our own common-controls manifest on this worker. The game does
    // not need to supply a window, activation context, UI system or render hook.
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&prompt),&self);
    ACTCTXW activation{sizeof(activation)};
    activation.dwFlags=ACTCTX_FLAG_HMODULE_VALID|ACTCTX_FLAG_RESOURCE_NAME_VALID;
    activation.hModule=self;activation.lpResourceName=MAKEINTRESOURCEW(2);
    const auto context=CreateActCtxW(&activation);
    ULONG_PTR cookie{};
    const bool activated=context!=INVALID_HANDLE_VALUE && ActivateActCtx(context,&cookie);
    const auto library=LoadLibraryExW(L"comctl32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Dialog=HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,int*,int*,BOOL*);
    const auto dialog=library?reinterpret_cast<Dialog>(GetProcAddress(library,"TaskDialogIndirect")):nullptr;
    int button=IDCANCEL;BOOL checked=FALSE;
    HRESULT outcome=E_FAIL;
    if(activated && dialog) {
        const TASKDIALOG_BUTTON buttons[]{
            {IDYES,L"Continue with CRML\nTry mods on this untested build"},
            {IDNO,L"Leave CRML disabled\nContinue playing without CRML mods"}};
        TASKDIALOGCONFIG config{sizeof(config)};
        PromptContext prompt_context{callback,callback_data};
        config.pfCallback=&prompt_event;config.lpCallbackData=reinterpret_cast<LONG_PTR>(&prompt_context);
        config.dwFlags=TDF_USE_COMMAND_LINKS|TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW|TDF_ENABLE_HYPERLINKS;
        config.pszWindowTitle=L"CONTROL Resonant Mod Loader";
        config.pszMainIcon=TD_WARNING_ICON;
        config.pszMainInstruction=L"CRML loaded. This game build is untested.";
        config.pszContent=L"A game update or a different edition can cause this. Check for a newer CRML release before trying mods. An update may restore support, but downloading the same version again will not change compatibility.\n\nMods might work normally, behave incorrectly, or crash. No mods or gameplay hooks have started. Continuing still checks hook signatures, but cannot guarantee compatibility.";
        config.cButtons=2;config.pButtons=buttons;config.nDefaultButton=IDNO;
        config.pszVerificationText=L"Remember my choice for this exact executable (don't ask again)";
        config.pszFooter=L"<a href=\"https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases\">Check for CRML updates on GitHub</a>\n\nClose the game before installing an update. To reset a remembered choice, remove crml\\compatibility-choice.txt with the game closed.";
        outcome=dialog(&config,&button,nullptr,&checked);
    }
    if(FAILED(outcome)) {
#ifdef CRML_COMPATIBILITY_TESTING
        if(callback) {
            if(library) FreeLibrary(library);
            if(activated) DeactivateActCtx(0,cookie);
            if(context!=INVALID_HANDLE_VALUE) ReleaseActCtx(context);
            return Choice::ask;
        }
#endif
        button=MessageBoxW(nullptr,L"CRML loaded, but this executable has not been tested. A game update or different edition can cause this. Check for a newer CRML release:\nhttps://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases\n\nClose the game before updating. Downloading the same version again does not change compatibility.\n\nMods may work, behave incorrectly, or crash. No mods or gameplay hooks will start until you choose Yes. Hook signature checks remain enabled.\n\nTry CRML for this session? Choose No to play without CRML mods.",
                           L"CRML - untested game build",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2|MB_SETFOREGROUND);
        checked=FALSE;
    }
    if(library) FreeLibrary(library);
    if(activated) DeactivateActCtx(0,cookie);
    if(context!=INVALID_HANDLE_VALUE) ReleaseActCtx(context);
    remember=checked && (button==IDYES || button==IDNO);
    return button==IDYES?Choice::allow:Choice::deny;
}
}
#ifdef CRML_COMPATIBILITY_TESTING
Choice test_prompt(bool& remember,PFTASKDIALOGCALLBACK callback,LONG_PTR data) {
    return prompt(remember,callback,data);
}
HRESULT test_prompt_link(HWND window,const wchar_t* target,OpenReleases open) {
    PromptContext context{};context.open=open;
    return prompt_event(window,TDN_HYPERLINK_CLICKED,0,reinterpret_cast<LPARAM>(target),reinterpret_cast<LONG_PTR>(&context));
}
#endif
Choice read_choice(const std::filesystem::path& path,std::string_view sha) {
    if(!valid_sha(sha)) return Choice::ask;
    std::ifstream file(path,std::ios::binary);
    char data[80]{};file.read(data,sizeof(data));
    const std::string_view record(data,static_cast<size_t>(file.gcount()));
    if(!file.eof() || record.size()<65 || record.substr(0,64)!=sha || record[64]!='\n') return Choice::ask;
    if(record.substr(65)=="allow\n") return Choice::allow;
    if(record.substr(65)=="deny\n") return Choice::deny;
    return Choice::ask;
}
bool write_choice(const std::filesystem::path& path,std::string_view sha,Choice choice) {
    if(!valid_sha(sha) || choice==Choice::ask) return false;
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file<<sha<<'\n'<<(choice==Choice::allow?"allow\n":"deny\n");file.flush();
    return bool(file);
}
bool authorize(const std::filesystem::path& root,std::ostream& log) {
    approved_sha[0]=0;
    reviewed_build=false;
    select_profile({});
    wchar_t path[32768]{};const auto length=GetModuleFileNameW(nullptr,path,32768);
    if(!length || length>=32768) {log<<"CRML startup refused: executable path unavailable\n";log.flush();return false;}
    // The standalone host/proxy tests are not game installations. Existing
    // per-service fingerprint checks continue to refuse native hooks there.
    if(_wcsicmp(std::filesystem::path(path).filename().c_str(),L"CONTROLResonant.exe")) return true;
    const auto sha=hash_file(path);
    log<<"CRML loaded; executable SHA-256: "<<sha<<'\n';log.flush();
    select_profile(sha);
    if(known_build(sha)) {reviewed_build=true;return true;}
    if(!valid_sha(sha)) {
        MessageBoxW(nullptr,L"CRML loaded, but could not identify the game executable. Mods have not been started.",
                    L"CRML - executable unavailable",MB_OK|MB_ICONWARNING);
        return false;
    }
    const auto choice_path=root/"compatibility-choice.txt";
    auto choice=read_choice(choice_path,sha);
    if(choice==Choice::ask) {
        bool remember{};choice=prompt(remember);
        if(remember && !write_choice(choice_path,sha,choice)) log<<"CRML compatibility choice could not be saved; will ask again next launch\n";
    }
    if(choice==Choice::allow) {
        std::memcpy(approved_sha,sha.data(),64);approved_sha[64]=0;
        log<<"Untested executable accepted by user; hook signature checks remain required\n";
    } else log<<"CRML disabled for this executable by user choice; no mods or gameplay hooks started\n";
    log.flush();return choice==Choice::allow;
}
}
