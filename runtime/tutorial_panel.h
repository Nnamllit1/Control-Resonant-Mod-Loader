#pragma once
#include "tutorial_context.h"
#include <cstdint>

namespace crml::tutorial {
inline constexpr uintptr_t panel_dispatch_rva=0x1e627f0;
inline constexpr uintptr_t panel_job_rva=0x1e62cc0;
inline constexpr uintptr_t panel_event_return_rva=0x1e629f9;
struct NativeStringView {const char* data{};uint64_t size{};};

// Trusted native adapter, never a guest-supplied call table. bind() requires
// the reviewed image and exact entry signatures. No pointer survives a scope
// except immutable code/global addresses within the process-pinned image.
struct PanelNative {
    uintptr_t image{},stack_name{},state_name{};
    void* (*stack)(const NativeStringView*,void*){};
    int32_t (*first)(const void*,const NativeStringView*){};
    bool (*active)(const void*,void*){};
    void (*close)(void*,void*){};
    static bool bind(uintptr_t image,PanelNative&) noexcept;
};
struct PanelCall {
    const void* query{};
    void* state{};
    void* stacks{};
    void* audio{};
    uintptr_t caller{};
};
// The registration ledger must supply this only while it owns a live native
// record. Matching these fields alone is NOT proof of registration ownership
// or protection against a destroyed/reused world or environment address.
struct PanelOwner {Identity identity{};uintptr_t state{};uint32_t key{};uint8_t mode{};};
enum class PanelClose : uint32_t {
    unavailable, scope, identity, foreign, pending, inactive, not_top,
    invalid, changed, await_sync, partial
};

// Borrowed whole-system event callback scope. The archetype job path is not
// admitted for mutation. Engine exceptions must unwind this TLS scope too.
class PanelScope {
public:
    PanelScope(const void* world_view,uint16_t id,const void* system,const PanelNative&) noexcept;
    ~PanelScope();
    PanelScope(const PanelScope&)=delete;
    PanelScope& operator=(const PanelScope&)=delete;
    static IdentityStatus identify(const PanelCall&,Identity&) noexcept;
    // Publish the stock secondary-open variant only in the verified mutable
    // environment boundary. Payload ownership is supplied by the native ledger.
    static bool open_owned(const PanelCall&,const PanelOwner&) noexcept;
    static bool release_owned(const PanelCall&,const PanelOwner&) noexcept;
    static PanelClose close_owned(const PanelCall&,const PanelOwner&);
    using Callback=void(*)(const void*,uint16_t,const void*);
    static void invoke(const void* world_view,uint16_t id,const void* system,const PanelNative&,Callback);
private:
    static void forward(PanelScope&,Callback);
    void leave() noexcept;
    bool valid(const PanelCall*) const noexcept;
    const PanelScope* previous_{};
    const void* world_view_{};
    const void* system_{};
    PanelNative native_{};
    uintptr_t world_{},state_{},stacks_{},audio_{};
    uint16_t id_{};
};
}
