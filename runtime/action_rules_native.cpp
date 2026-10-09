#include "action_rules.h"
namespace crml::action_rules {
namespace {
uint64_t clock() noexcept {return GetTickCount64();}
uint64_t allowed(const Identity& context,uint64_t now) noexcept {return process_service().allowed(context,now);}
bool pending(uint64_t now) noexcept {return process_service().pending(now);}
}
Service& process_service() noexcept {
    static Service service(&action_restriction_observer::identity,&clock);
    return service;
}
bool start() noexcept {
    const bool ready=action_restriction_observer::start_tracking(&allowed,&pending);
    process_service().available(ready);
    return ready;
}
void stop() noexcept {
    process_service().available(false);
    action_restriction_observer::stop_tracking();
}
}
