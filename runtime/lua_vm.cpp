#include "lua_vm.h"
#include <Windows.h>

namespace crml::engine::lua {
// Read only a bounded error object before stack cleanup. Never emit its text:
// engine errors may contain asset paths, identifiers or other private data.
Error error_details(void* vm,const char* label) noexcept {
    Error result;
    result.kind="unavailable";
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        const auto top=read<uintptr_t>(l+8),base=read<uintptr_t>(l+0x10);
        if(top<base || top-base<24 || read<uint32_t>(top-8)!=5) return result;
        const auto string=read<uintptr_t>(top-24);
        if(!string || read<uint8_t>(string)!=5) return result;
        const auto size=read<uint32_t>(string+0x14);
        if(!size || size>4096) return result;
        char text[513]{};
        std::memcpy(text,reinterpret_cast<const void*>(string+0x18),size<512?size:512);
        const char* name=label[0]=='='?label+1:label;
        const auto length=std::strlen(name);
        const char* start=text[0]=='='?text+1:text;
        if(!std::strncmp(start,name,length) && start[length]==':') {
            const char* cursor=start+length+1;
            unsigned line=0,digits=0;
            while(*cursor>='0' && *cursor<='9' && digits<5) {line=line*10+unsigned(*cursor++-'0');++digits;}
            if(digits && *cursor==':' && line) result.line=line;
        }
        result.kind=std::strstr(text,"no active script")?"no_active_script":
            std::strstr(text,"required ECS environment missing")?"missing_ecs":
            std::strstr(text,"invalid event handler handle")?"invalid_handler":
            std::strstr(text,"attempt to call a nil value")?"nil_call":
            std::strstr(text,"assertion failed")?"assertion":"other";
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        result.kind="unavailable";result.line=0;
    }
    return result;
}
// No API calls here; a bad layout only rejects this observation.
bool capture(void* vm,Frame& f,uintptr_t spare) noexcept {
    __try {
        const auto l=reinterpret_cast<uintptr_t>(vm);
        if(!l || read<uint8_t>(l+3)) return false;
        const auto stack=read<uintptr_t>(l+0x30),base=read<uintptr_t>(l+0x10);
        const auto top=read<uintptr_t>(l+8),last=read<uintptr_t>(l+0x28);
        const auto ci=read<uintptr_t>(l+0x20),baseci=read<uintptr_t>(l+0x40);
        if(!stack || base<stack || top<=base || last<top || last-top<spare ||
           spare>f.values.size() || top-base>f.values.size()-spare || (top-stack)%24 || (base-stack)%24 ||
           !ci || ci!=baseci || read<uintptr_t>(ci)!=base) return false;
        if(read<uint32_t>(base+0x10)!=7) return false; // Existing engine error handler.
        f.context=read<uintptr_t>(l+0x78);
        f.global=read<uintptr_t>(l+0x18);f.environment=read<uintptr_t>(l+0x58);
        if(!f.context || !f.global || !f.environment) return false;
        f.world=read<uintptr_t>(f.context);
        // A debugger may suspend on the deliberate error instead of unwinding.
        if(!f.world || read<uintptr_t>(f.global+0xd28)) return false;
        f.top=top-stack;f.base=base-stack;f.ci=ci-baseci;f.size=top-base;
        std::memcpy(f.values.data(),reinterpret_cast<void*>(base),f.size);
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
bool same_frame(const Frame& a,const Frame& b,bool top) noexcept {
    return a.context==b.context && a.world==b.world && a.global==b.global &&
        a.environment==b.environment && a.base==b.base && a.ci==b.ci && (!top ||
        (a.top==b.top && a.size==b.size && !std::memcmp(a.values.data(),b.values.data(),a.size)));
}
bool valid_owner(Owner owner) noexcept {
    __try {
        const uint32_t index=static_cast<uint32_t>(owner.entity),generation=static_cast<uint32_t>(owner.entity>>32);
        if(!owner.world || !owner.entity || index==UINT32_MAX || index>=read<uint64_t>(owner.world+0x58510)) return false;
        const auto slots=read<uintptr_t>(owner.world+0x584e8);
        return slots && read<uint32_t>(slots+uintptr_t(index)*8)==generation;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
}
