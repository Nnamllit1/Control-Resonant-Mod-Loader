#include "lua_executor.h"

namespace crml::engine::lua {
namespace {
bool chunk_name(const char* label) noexcept {
    if(!label || label[0]!='=' || !label[1]) return false;
    for(size_t i=1;i<128;++i) {
        const unsigned char c=static_cast<unsigned char>(label[i]);
        if(!c) return true;
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
             (c>='0' && c<='9') || c=='_' || c=='-' || c=='.')) return false;
    }
    return false;
}
bool ready(const Api& api,Action action,int reference,Program program,Options options,Liveness live) noexcept {
    if(!live.vm_live || !live.owner_live || !api.protect || !api.settop) return false;
    if(options.returns!=Returns::discard && options.returns!=Returns::number) return false;
    if(program.label && !chunk_name(program.label)) return false;
    switch(action) {
    case Action::initialize:
        // A format check is not authentication or a sandbox. Only a trusted
        // host may supply compiler output; never pass arbitrary downloaded bins.
        return !reference && chunk_name(program.label) && program.bytes.size()>=3 &&
            program.bytes.size()<=16*1024*1024 && program.bytes[0]==6 && program.bytes[1]==3 &&
            options.environment_arguments<=3 && api.load && api.call && api.push_entity &&
            api.new_table && api.push_value && api.set_field && api.readonly &&
            api.set_metatable && api.retain && (!options.global_must_be_nil || api.raw_field);
    case Action::invoke:
        return reference>0 && chunk_name(program.label) && api.fetch && api.call;
    case Action::unload:
        return reference>0 && chunk_name(program.label) && api.fetch && api.call && api.push_value && api.release;
    case Action::release:
        return reference>0 && api.release;
    }
    return false;
}
struct Work {
    const Api& api;
    Context context;
    Action action;
    int reference;
    Program program;
    Options options;
    Liveness live;
    Execution result;
};
void returned(Work& w,void* vm,bool shutdown) {
    auto& result=w.result;
    if(result.status) {result.error=error_details(vm,w.program.label);return;}
    if(w.options.returns==Returns::number) {
        const auto value=read<uintptr_t>(reinterpret_cast<uintptr_t>(vm)+8)-24;
        if(read<uint32_t>(value+16)!=3) {result.status=shutdown?-307:-302;return;}
        result.value=read<double>(value);
    }
    result.shutdown=shutdown;
}
void body(void* vm,void* user) {
    auto& w=*static_cast<Work*>(user);
    auto& result=w.result;const auto& api=w.api;
    if(!w.live.owner_live()) return;
    const auto l=reinterpret_cast<uintptr_t>(vm);
    const int saved=static_cast<int>((read<uintptr_t>(l+8)-read<uintptr_t>(l+0x10))/24);
    const int results=w.options.returns==Returns::number?1:0;
    if(w.action==Action::release || w.action==Action::unload) {
        if(w.action==Action::unload) {
            const int tag=api.fetch(vm,-10000,w.reference);
            if(!w.live.owner_live()) return;
            if(tag!=7) result.status=-301;
            else {
                // The controller treats a truthy command as shutdown. Copy an
                // already-rooted closure; do not allocate an unrooted token.
                api.push_value(vm,-1);
                if(!w.live.owner_live()) return;
                result.status=api.call(vm,1,results,0);
                if(!w.live.owner_live()) return;
                returned(w,vm,true);
            }
        }
        result.release_attempted=true;
        api.release(vm,w.reference);result.released=true;return;
    }
    if(w.action==Action::invoke) {
        const int tag=api.fetch(vm,-10000,w.reference);
        if(!w.live.owner_live()) return;
        if(tag!=7) {result.status=-301;return;}
        result.status=api.call(vm,0,results,0);
        if(!w.live.owner_live()) return;
        returned(w,vm,false);return;
    }
    if(w.options.global_must_be_nil) {
        const int tag=api.raw_field(vm,-10002,w.options.global_must_be_nil);
        if(!w.live.owner_live()) return;
        if(tag!=0) {result.status=-303;return;}
        api.settop(vm,saved);
        if(!w.live.owner_live()) return;
    }
    api.new_table(vm,0,3);const int env=saved+1;
    if(!w.live.owner_live()) return;
    api.new_table(vm,0,1);
    if(!w.live.owner_live()) return;
    api.push_value(vm,-10002);
    if(!w.live.owner_live()) return;
    api.set_field(vm,-2,"__index");
    if(!w.live.owner_live()) return;
    api.readonly(vm,-1,1);
    if(!w.live.owner_live()) return;
    const int attached=api.set_metatable(vm,env);
    if(!w.live.owner_live()) return;
    if(!attached) {result.status=-304;return;}
    api.push_value(vm,env);
    if(!w.live.owner_live()) return;
    api.set_field(vm,env,"_ENV");
    if(!w.live.owner_live()) return;
    api.push_entity(vm,w.context.owner,1);
    if(!w.live.owner_live()) return;
    api.set_field(vm,env,"self");
    if(!w.live.owner_live()) return;
    result.status=api.load(vm,w.program.label,reinterpret_cast<const char*>(w.program.bytes.data()),w.program.bytes.size(),env);
    if(!w.live.owner_live()) return;
    if(result.status) {result.error=error_details(vm,w.program.label);return;}
    for(unsigned i=0;i<w.options.environment_arguments;++i) {
        api.push_value(vm,env);
        if(!w.live.owner_live()) return;
    }
    result.status=api.call(vm,static_cast<int>(w.options.environment_arguments),1,0);
    if(!w.live.owner_live()) return;
    if(result.status) {result.error=error_details(vm,w.program.label);return;}
    if(read<uint32_t>(read<uintptr_t>(l+8)-8)!=7) {result.status=-305;return;}
    // Retain before the first invocation. A constructor that acquires resources
    // cannot rely on a subsequent retain succeeding; this is a trusted contract.
    result.reference=api.retain(vm,-1);
    if(result.reference<=0) result.status=-306;
}
}
Execution execute(const Api& api,Context context,Action action,int reference,Program program,Options options,Liveness live) {
    if(!ready(api,action,reference,program,options,live)) {Execution result;result.status=-309;return result;}
    if(!live.vm_live() || !live.owner_live()) return {};
    Frame initial;
    if(!capture(context.vm,initial,144) || initial.global!=context.global || initial.world!=context.world ||
       !valid_owner({context.world,context.owner})) return {};
    Work work{api,context,action,reference,program,options,live,{}};work.result.attempted=true;
    const int status=api.protect(context.vm,&body,&work,initial.top,0);
    if(status) work.result.status=status;
    // Readability does not establish liveness: a closed address can be reused.
    if(!live.vm_live()) return work.result;
    if(status && live.owner_live() && program.label) work.result.error=error_details(context.vm,program.label);
    Frame before_cleanup;
    if(capture(context.vm,before_cleanup,0) && same_frame(initial,before_cleanup,false) && before_cleanup.top>=initial.top) {
        api.settop(context.vm,static_cast<int>((initial.top-initial.base)/24));
        if(!live.vm_live()) return work.result;
        Frame after;
        work.result.restored=capture(context.vm,after) && same_frame(initial,after,true);
    }
    return work.result;
}
}
