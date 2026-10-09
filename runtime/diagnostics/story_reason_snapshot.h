#pragma once
#include "script_origin.h"
#include <cmath>

namespace crml::action_restriction_observer {
// Copied arguments of the reviewed manager at its native binding call. This is
// an observation of requested mode, not an applied result or a player lease.
struct StoryReasons {
    std::array<uint32_t,6> counts{};
    uint8_t active_mask{};
    bool enabled{};
};
inline bool copy_story_reasons(void* state,StoryReasons& output) noexcept {
    output={};
    __try {
        using probe::script_origin::detail::read;
        using probe::script_origin::detail::label;
        const auto vm=reinterpret_cast<uintptr_t>(state);
        if(!vm || read<uint8_t>(vm+3)) return false;
        const auto stack=read<uintptr_t>(vm+0x30),last=read<uintptr_t>(vm+0x28);
        const auto base=read<uintptr_t>(vm+0x10),top=read<uintptr_t>(vm+8);
        const auto ci=read<uintptr_t>(vm+0x20),first=read<uintptr_t>(vm+0x40);
        if(!stack || base<stack || top<base || last<top || top-base!=24 ||
           (base-stack)%24 || !first || ci<first || (ci-first)%0x28 ||
           (ci-first)/0x28<1 || (ci-first)/0x28>1024 || read<uintptr_t>(ci)!=base ||
           read<uint32_t>(base+0x10)!=1) return false;
        const auto flag=read<int32_t>(base);
        if(flag!=0 && flag!=1) return false;
        // Only the immediate Lua caller is accepted; a similarly named outer
        // frame must not attribute another script's native call to this manager.
        const auto parent=ci-0x28;
        const auto values=read<uintptr_t>(parent),function=read<uintptr_t>(parent+8);
        if(values<stack || values>base || base-values<6*24 || (values-stack)%24 ||
           function<stack || function>=values || values-function<24 ||
           (function-stack)%24 || read<uint32_t>(function+0x10)!=7) return false;
        const auto closure=read<uintptr_t>(function);
        if(!closure || read<uint8_t>(closure)!=7 || read<uint8_t>(closure+3)) return false;
        const auto proto=read<uintptr_t>(closure+0x18);
        if(!proto || read<uint32_t>(proto+0xa4)!=26 || read<uint32_t>(proto+0xa8)!=1 ||
           read<uint32_t>(proto+0x88)!=96) return false;
        std::array<char,96> source{};
        std::array<char,64> name{};
        if(!label(read<uintptr_t>(proto+0x58),source,true) ||
           !label(read<uintptr_t>(proto+0x60),name,false) ||
           std::strcmp(source.data(),"story_mode_reasons_manager.lua") ||
           std::strcmp(name.data(),"evaluate_story_mode_reasons")) return false;
        StoryReasons result{};
        for(size_t i=0;i<result.counts.size();++i) {
            const auto cell=values+i*24;
            if(read<uint32_t>(cell+0x10)!=3) return false;
            const auto number=read<double>(cell);
            if(!std::isfinite(number) || number<0 || number>UINT32_MAX || std::floor(number)!=number) return false;
            result.counts[i]=static_cast<uint32_t>(number);
            if(number>0) result.active_mask|=uint8_t(1u<<i);
        }
        result.enabled=flag!=0;
        if(result.enabled!=(result.active_mask!=0)) return false;
        output=result;
        return true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
}
