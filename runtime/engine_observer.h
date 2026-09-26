#pragma once
#include "movement_view.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace crml::observer {
enum class Kind : uint8_t { movement, command_flush, script_fixed, renderer_sync,
    physics_begin, physics_wait, physics_complete, player, resource, post_physics, body, body_scan, entity_body, entity_scan, count };
inline constexpr const char* names[]{"movement","command_flush","script_fixed","renderer_sync",
    "physics_begin","physics_wait","physics_complete","player","resource","post_physics","body","body_scan","entity_body","entity_scan"};
struct Event {
    uint64_t sequence{}, qpc{}, span{}, object{}, entity{}, value{}, detail{};
    uint32_t thread{}, flags{};
    Kind kind{};
    uint8_t edge{}; // 0 observation, 1 enter, 2 normal return. Missing return is not success.
};
// No allocation, waiting, file IO, or retained engine pointers on producer threads.
// Contention/full-buffer losses are explicit; sequence is publication order.
class Buffer {
public:
    static constexpr size_t capacity = 8192;
    void open() noexcept;
    void close() noexcept;
    bool push(Event event) noexcept;
    size_t drain(Event* output, size_t limit) noexcept;
    uint64_t dropped() const noexcept { return dropped_.load(); }
    uint64_t count(Kind kind) const noexcept { return counts_[static_cast<size_t>(kind)].load(); }
private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    bool accepting_{};
    size_t head_{}, size_{};
    uint64_t sequence_{};
    std::array<Event, capacity> events_{};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Kind::count)> counts_{};
    std::atomic<uint64_t> dropped_{};
};
struct ResourceSample { bool readable{}; uintptr_t pointer{}; uint64_t id{}; uint32_t refs{}, state{}; };
// Snapshot only: neither retains a resource nor follows it later on the worker.
ResourceSample read_resource(uintptr_t pointer) noexcept;
uint64_t identity(uintptr_t pointer, uint64_t salt) noexcept;
void write_event(std::ostream& stream, const Event& event);
#ifdef CRML_OBSERVER_TESTING
bool test_hook_prologues();
#endif

class Recorder {
public:
    std::string start(const std::filesystem::path& root);
    void poll();
    bool active() const noexcept { return output_.is_open(); }
    ~Recorder();
private:
    bool line(const std::string& text);
    void finish(const char* reason);
    std::ofstream output_;
    uint64_t bytes_{}, started_{}, last_stats_{}, written_{};
    std::array<Event, 512> batch_{};
};
}
