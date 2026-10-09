#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace crml::dialogue {
// Private diagnostic data, not an SDK event. Selection can subsequently be
// filtered, and fact publication is not renderer presentation or audibility.
enum class Read : uint32_t { ok, empty, arguments, identity, range, encoding, memory };
enum class Stage : uint32_t { selected, published };
struct Observation {
    uint32_t allocation{}, playback_key{}, segment{UINT32_MAX}, length{}, forced{};
    float elapsed{}, duration{};
    char text[4097]{};
};
// Caller must own these objects inside the authenticated native producer call.
// No engine calls or allocation; all native pointers are discarded before return.
Read inspect_selected(const void* ui_record,const void* source_record,Observation&) noexcept;
Read inspect_published(const void* subtitle_state,uint32_t slot,Observation&) noexcept;
uint64_t text_hash(const Observation&) noexcept;
}
