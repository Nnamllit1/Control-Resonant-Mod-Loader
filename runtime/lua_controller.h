#pragma once
#include "lua_dispatch.h"
#include "lua_executor.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace crml::engine::lua {
bool valid_controller_id(std::string_view id) noexcept;
// Internal host for trusted, already-compiled controllers. Filesystem discovery
// and source compilation belong off the engine thread and are not performed here.
class ControllerHost {
public:
    using Execute=Execution(*)(const Api&,Context,Action,int,Program,Options,Liveness);
    static constexpr size_t capacity=16,max_program_bytes=16*1024*1024,max_total_bytes=64*1024*1024;
    struct Snapshot {
        size_t packages{},references{},pending{},failed{},awaiting_owner{};
        uint64_t initialized{},invoked{},released{},reclaimed{},failures{},rejected{},interrupted{},ambiguous_releases{};
        bool configured{},halted{};
    };
    struct PackageStatus {
        std::string id;
        const char* state;
        uint64_t active_revision{},desired_revision{},calls{};
        int status{};
        unsigned line{};
    };
    explicit ControllerHost(Api api,Execute provider=&execute) noexcept;
    // Keep the host alive until stop() has drained or its VMs have closed. The
    // destructor disconnects notifications; it cannot run Lua on a worker thread.
    ~ControllerHost();
    ControllerHost(const ControllerHost&)=delete;
    ControllerHost& operator=(const ControllerHost&)=delete;
    bool submit(std::string id,std::vector<unsigned char> bytecode,unsigned interval_ms=16);
    bool unload(std::string_view id) noexcept;
    void stop() noexcept;
    void tick(Context,uint64_t now);
    Snapshot snapshot() const noexcept;
    std::vector<PackageStatus> packages() const;
private:
    struct Code {std::vector<unsigned char> bytes;std::string label;uint64_t generation{};unsigned interval{};};
    struct Slot {
        std::string id;
        std::shared_ptr<const Code> desired,active;
        uintptr_t global{},world{};
        uint64_t owner{},next_tick{},failed_generation{};
        int reference{};
        bool retired{},engine_retired{},blocked_owner{};
        uint64_t calls{};
        int last_status{};
        unsigned error_line{};
    };
    Api api_;
    Execute execute_;
    bool configured_{},halted_{};
    uint64_t serial_{};
    std::array<Slot,capacity> slots_{};
    Snapshot totals_{};
    static void notify(void*,dispatch::Event) noexcept;
    void notification(dispatch::Event) noexcept;
    void step(Slot&,Context,Action,uint64_t);
};
}
