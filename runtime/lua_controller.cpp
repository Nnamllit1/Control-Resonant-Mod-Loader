#include "lua_controller.h"
#include <limits>

namespace crml::engine::lua {
bool valid_controller_id(std::string_view id) noexcept {
    if(id.empty() || id.size()>64) return false;
    for(const unsigned char c:id)
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-')) return false;
    return true;
}
ControllerHost::ControllerHost(Api api,Execute provider) noexcept:api_(api),execute_(provider) {
    configured_=provider && dispatch::subscribe(this,&notify);
}
ControllerHost::~ControllerHost() {if(configured_) dispatch::unsubscribe(this);}
bool ControllerHost::submit(std::string id,std::vector<unsigned char> bytes,unsigned interval) {
    if(!valid_controller_id(id) || bytes.size()<3 || bytes.size()>max_program_bytes || bytes[0]!=6 || bytes[1]!=3 ||
       !interval || interval>60000) return false;
    auto code=std::make_shared<Code>(Code{std::move(bytes),"=crml_"+id,0,interval});
    dispatch::Gate lock;
    if(!configured_ || halted_ || dispatch::halted() || serial_==std::numeric_limits<uint64_t>::max()) return false;
    Slot* target=nullptr;
    for(auto& slot:slots_) if(slot.id==id) {target=&slot;break;}
    if(!target) for(auto& slot:slots_) if(slot.id.empty()) {target=&slot;break;}
    if(!target) return false;
    size_t total=0;
    for(const auto& slot:slots_) {
        const Code* desired=&slot==target?code.get():slot.desired.get();
        if(desired) total+=desired->bytes.size();
        if(slot.active && slot.active.get()!=desired) total+=slot.active->bytes.size();
    }
    if(total>max_total_bytes) return false;
    code->generation=++serial_;
    target->id=std::move(id);target->desired=std::move(code);
    return true;
}
bool ControllerHost::unload(std::string_view id) noexcept {
    dispatch::Gate lock;
    for(auto& slot:slots_) if(slot.id==id && !id.empty()) {slot.desired.reset();return true;}
    return false;
}
void ControllerHost::stop() noexcept {dispatch::Gate lock;for(auto& slot:slots_) slot.desired.reset();}
void ControllerHost::notify(void* host,dispatch::Event event) noexcept {static_cast<ControllerHost*>(host)->notification(event);}
void ControllerHost::notification(dispatch::Event event) noexcept {
    if(!event.global) {halted_=true;++totals_.failures;return;}
    for(auto& slot:slots_) if(slot.global==event.global && (event.close || slot.owner==event.owner)) {
        slot.engine_retired=true;slot.blocked_owner=false;
        if(event.close) {
            if(slot.reference>0) ++totals_.reclaimed;
            slot.reference=0;slot.global=0;slot.world=0;slot.owner=0;slot.retired=false;
        } else if(slot.reference>0) slot.retired=true;
    }
}
void ControllerHost::step(Slot& slot,Context context,Action action,uint64_t now) {
    // Retain the exact source revision until the operation returns, even if a
    // same-thread callback queues its replacement while Lua is executing.
    const auto code=slot.active;
    dispatch::Operation operation(context,this);
    Execution result;
    try {
        result=execute_(api_,context,action,slot.reference,{code->bytes,code->label.c_str()},{},
            {&dispatch::call_vm_live,&dispatch::call_owner_live});
    } catch(...) {halted_=true;dispatch::halt();++totals_.failures;throw;}
    const auto interruption=operation.interruption;
    if(interruption!=dispatch::Interruption::none) ++totals_.interrupted;
    if(interruption==dispatch::Interruption::closed) {
        // The close notification already reclaimed any published reference.
        // A reference returned by an interrupted constructor was not published.
        if(action==Action::initialize && result.reference>0) ++totals_.reclaimed;
        return;
    }
    if(interruption==dispatch::Interruption::unknown) return;
    if(!result.attempted) {++totals_.rejected;return;}
    if(interruption==dispatch::Interruption::none && (action==Action::initialize || result.status || !result.restored)) {
        slot.last_status=result.status?result.status:result.restored?0:-311;slot.error_line=result.error.line;
    }
    bool failed=false;
    const auto fail=[&] {if(!failed) {failed=true;++totals_.failures;}};
    if(!result.restored) {halted_=true;dispatch::halt();fail();}
    if(action==Action::initialize) {
        slot.calls=0;
        slot.reference=result.reference>0?result.reference:0;
        slot.next_tick=now+code->interval;
        if(interruption==dispatch::Interruption::owner) {slot.retired=slot.reference>0;return;}
        if(!result.status && result.restored && slot.reference>0) ++totals_.initialized;
        else {
            fail();slot.failed_generation=code->generation;
            slot.retired=slot.reference>0;
        }
    } else if(action==Action::invoke) {
        if(interruption!=dispatch::Interruption::none) return;
        slot.next_tick=now+code->interval;
        if(!result.status && result.restored) {++totals_.invoked;++slot.calls;}
        else {fail();slot.failed_generation=code->generation;slot.retired=true;}
    } else {
        if(interruption==dispatch::Interruption::owner && !result.released && !result.release_attempted) {
            slot.retired=true;return; // Later raw release; never repeat shutdown.
        }
        if(!result.release_attempted && !result.released) {
            // No trusted evidence that shutdown completed, and no teardown
            // explained the early return. Do not call an uncertain handle again.
            halted_=true;dispatch::halt();fail();return;
        }
        slot.reference=0;slot.retired=false;
        if(result.released) ++totals_.released;
        else {++totals_.ambiguous_releases;slot.blocked_owner=true;}
        if(result.status || !result.restored || !result.released ||
           (action==Action::unload && interruption==dispatch::Interruption::none && !result.shutdown)) {
            fail();slot.failed_generation=code->generation;
            // An unsuccessful shutdown can leave engine-owned resources. A new
            // source revision must wait for owner cleanup before starting.
            slot.blocked_owner=!slot.engine_retired;
        }
        slot.active.reset();
    }
}
void ControllerHost::tick(Context context,uint64_t now) {
    if(!configured_ || !context.vm || !context.global || !context.world || !context.owner) return;
    dispatch::Gate lock(false);
    if(!lock.held || halted_ || dispatch::halted() || dispatch::teardown_depth() || context.revision!=dispatch::revision()) return;
    for(auto& slot:slots_) {
        if(slot.id.empty()) continue;
        if(slot.global && slot.global!=context.global) continue;
        if(slot.world && slot.world!=context.world) {slot.engine_retired=true;slot.retired=slot.reference>0;slot.blocked_owner=false;}
        if(slot.reference>0) {
            if(slot.desired!=slot.active) slot.retired=true;
            if(slot.retired) {
                if(!slot.engine_retired && context.owner!=slot.owner) continue;
                step(slot,context,slot.engine_retired?Action::release:Action::unload,now);
            } else if(context.owner==slot.owner && now>=slot.next_tick) step(slot,context,Action::invoke,now);
        } else if(!slot.blocked_owner) {
            if(!slot.desired) {slot={};continue;}
            if(slot.desired->generation==slot.failed_generation) continue;
            if(slot.owner && !slot.engine_retired && slot.owner!=context.owner) continue;
            slot.active=slot.desired;slot.global=context.global;slot.world=context.world;slot.owner=context.owner;
            slot.retired=false;slot.engine_retired=false;
            step(slot,context,Action::initialize,now);
        }
        // Teardown during one package must not pass this old context to another.
        if(halted_ || context.revision!=dispatch::revision()) break;
    }
}
ControllerHost::Snapshot ControllerHost::snapshot() const noexcept {
    dispatch::Gate lock;
    auto result=totals_;result.configured=configured_;result.halted=halted_ || dispatch::halted();
    for(const auto& slot:slots_) if(!slot.id.empty()) {
        ++result.packages;
        if(slot.reference>0) ++result.references;
        if(slot.desired!=slot.active || slot.retired ||
           (!slot.reference && slot.desired && slot.desired->generation!=slot.failed_generation)) ++result.pending;
        if(slot.failed_generation && (!slot.desired || slot.desired->generation==slot.failed_generation)) ++result.failed;
        if(slot.blocked_owner) ++result.awaiting_owner;
    }
    return result;
}
std::vector<ControllerHost::PackageStatus> ControllerHost::packages() const {
    dispatch::Gate lock;
    std::vector<PackageStatus> result;
    for(const auto& slot:slots_) if(!slot.id.empty()) {
        const char* state=halted_ || dispatch::halted()?"halted":slot.blocked_owner?"awaiting_owner":
            slot.retired?"retiring":slot.reference>0?"active":
            slot.desired && slot.desired->generation==slot.failed_generation?"failed":slot.desired?"queued":"stopped";
        result.push_back({slot.id,state,slot.active?slot.active->generation:0,slot.desired?slot.desired->generation:0,
            slot.calls,slot.last_status,slot.error_line});
    }
    return result;
}
}
