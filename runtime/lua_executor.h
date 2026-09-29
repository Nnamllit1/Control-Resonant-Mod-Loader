#pragma once
#include "lua_vm.h"
#include <span>

namespace crml::engine::lua {
enum class Action {initialize,invoke,unload,release};
struct Program {
    std::span<const unsigned char> bytes;
    const char* label{}; // Stable, bounded chunk name, never a filesystem path.
};
enum class Returns {discard,number};
struct Options {
    Returns returns{Returns::discard};
    // Optional constructor arguments are copies of its rooted environment.
    // The embedded diagnostics use these; normal controllers need no arguments.
    unsigned environment_arguments{};
    const char* global_must_be_nil{};
};
struct Liveness {
    // Both callbacks are required and describe this operation under its host's
    // teardown gate. A true return alone does not acquire or authenticate a VM.
    bool(*vm_live)() noexcept{};
    bool(*owner_live)() noexcept{};
};
struct Execution {
    bool attempted{},restored{},released{};
    int reference{},status{};
    double value{};
    bool shutdown{},release_attempted{};
    Error error;
};
// Borrowed arguments must remain valid for the whole synchronous call. The host
// authenticates the engine, provides an eligible engine-thread frame, serializes
// teardown and owns registry references across calls. This function schedules
// nothing, reads no files, owns no VM and does not sandbox trusted engine Lua.
// Constructors must defer engine resource acquisition until after retention.
Execution execute(const Api&,Context,Action,int reference,Program,Options,Liveness);
}
