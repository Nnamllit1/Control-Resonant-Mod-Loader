#pragma once
#include <cstdint>
#include "tutorial_requests.h"

namespace crml::tutorial {
// Internal ABI for the fingerprinted October hotfix. These are borrowed engine
// inputs, never guest handles or save/campaign identities.
enum class Dispatch { direct, archetype };
enum class IdentityStatus : uint32_t { unavailable, ok, scope, query, entity, components, changed, memory };
struct Identity {uintptr_t world{};uint64_t entity{};};
inline constexpr uintptr_t request_dispatch_rva=0x2042c70;
inline constexpr uintptr_t request_job_rva=0x20444a0;

// Thread-local dynamic scope: nested calls shadow the outer world even when
// rejected. A scope confers no mutation permission and owns no engine objects.
class RequestScope {
public:
    RequestScope(const void* world_view,uint16_t id,const void* system,Dispatch,uintptr_t image) noexcept;
    ~RequestScope();
    RequestScope(const RequestScope&)=delete;
    RequestScope& operator=(const RequestScope&)=delete;
    static IdentityStatus identify(const void* query,Identity&) noexcept;
    // Caller must own the registered key. This is queue withdrawal only, not
    // permission to dispose its payload; active presentation can outlive it.
    static Withdrawal withdraw(const void* query,Identity expected,uint32_t owned_key) noexcept;
    using Callback=void(*)(const void*,uint16_t,const void*);
    // Native dispatchers must use this entry: /EHsc alone does not unwind a
    // C++ destructor when an engine SEH exception is handled by an outer frame.
    static void invoke(const void* world_view,uint16_t id,const void* system,Dispatch,uintptr_t image,Callback);
private:
    static void forward(RequestScope&,Callback);
    void leave() noexcept;
    const RequestScope* previous_{};
    const void* world_view_{};
    const void* system_{};
    uintptr_t image_{},world_{};
    uint16_t id_{};
    Dispatch dispatch_{};
};
}
