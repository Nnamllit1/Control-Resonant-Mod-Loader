#include <Windows.h>
#include <MinHook.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

using Function=int(*)();
Function original{};
int detour(){return original()+1;}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try {
    require(MH_Initialize()==MH_OK,"initialize hook library");
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    require(memory!=nullptr,"allocate fixture");
    // A complete ten-byte scalar prologue, then AVX that must stay in place.
    const unsigned char code[]{0x4c,0x8b,0xdc,0x48,0x81,0xec,0x18,0x01,0,0,
        0xc5,0xf8,0x57,0xc0,0x48,0x81,0xc4,0x18,0x01,0,0,0xb8,17,0,0,0,0xc3};
    std::memcpy(memory,code,sizeof(code));
    // AVX at entry and after a short prologue must still be rejected: these
    // instructions would need relocation, beyond the supported decoder.
    const unsigned char unsupported[]{0xc5,0xf8,0x57,0xc0,0xc3};
    std::memcpy(memory+128,unsupported,sizeof(unsupported));
    memory[256]=0x90;std::memcpy(memory+257,unsupported,sizeof(unsupported));
    DWORD protection{};require(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&protection)!=0,"protect fixture");
    FlushInstructionCache(GetCurrentProcess(),memory,4096);
    void* rejected{};
    require(MH_CreateHook(memory+128,reinterpret_cast<void*>(&detour),&rejected)==MH_ERROR_UNSUPPORTED_FUNCTION,"unsupported entry refused");
    require(MH_CreateHook(memory+256,reinterpret_cast<void*>(&detour),&rejected)==MH_ERROR_UNSUPPORTED_FUNCTION,"unsupported displaced instruction refused");
    require(MH_CreateHook(memory,reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&original))==MH_OK,"jump-back boundary accepted");
    require(MH_EnableHook(memory)==MH_OK,"enable boundary hook");
    require(std::memcmp(memory+10,code+10,sizeof(code)-10)==0,"AVX tail is untouched");
    if(IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE)) {
        require(original()==17,"trampoline returns through original AVX tail");
        require(reinterpret_cast<Function>(memory)()==18,"detour calls original once");
    }
    require(MH_DisableHook(memory)==MH_OK,"disable boundary hook");
    require(std::memcmp(memory,code,sizeof(code))==0,"restores original code");
    require(MH_RemoveHook(memory)==MH_OK,"remove boundary hook");
    VirtualFree(memory,0,MEM_RELEASE);MH_Uninitialize();
    std::cout<<"Hook boundary checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
