#pragma once
#include "mod_metadata.h"
#include "map_projection.h"
#include "sonar_projection.h"
#include "../sdk/include/crml_drawing.h"
#include <functional>
#include <memory>
#include <vector>

namespace crml {
inline constexpr std::string_view drawing_prefix="coui://base/crml/drawing/v1/";
class ModDrawing {
public:
    using Clock=std::function<uint64_t()>;
    struct Surface {uint64_t owner,revision;uint32_t remaining_ms;crml_drawing_frame frame;uint32_t target{};};
    explicit ModDrawing(Clock clock={});
    ~ModDrawing();
    ModDrawing(const ModDrawing&)=delete;
    ModDrawing& operator=(const ModDrawing&)=delete;
    bool attach(uint64_t owner,std::string_view id,const ModMetadata& metadata={});
    void detach(uint64_t owner);
    void cancel(uint64_t owner);
    bool available();
    void enable_renderer(bool enabled);
    uint64_t open_page();
    // 1 admitted (not presentation proof), -1 unavailable, -2 shared capacity,
    // -3 invalid, -4 rate (one per100ms), -5 resource/revision exhaustion.
    int publish(uint64_t owner,const crml_drawing_frame& frame);
    // Host state clears immediately. Pixels clear on the next renderer update.
    // 1 changed,0 already hidden,-1 unknown owner. Bypasses publish rate limits.
    int hide(uint64_t owner);
    // Called only with copied native data. Never waits on a rendering callback.
    void update_map(const map_projection::District&,const std::array<float,4>&) noexcept;
    void update_sonar(const sonar_projection::Snapshot&) noexcept;
    int map_read(uint64_t owner,crml_map_state&);
    int projection_read(uint64_t owner,crml_map_projection&);
    int map_read_target(uint64_t owner,uint32_t target,crml_map_state&);
    int map_publish(uint64_t owner,const crml_map_frame&);
    int map_hide(uint64_t owner);
    int map_hide_target(uint64_t owner,uint32_t target);
    int annotations_publish(uint64_t owner,const crml_map_annotations&);
    int annotations_publish_v2(uint64_t owner,const crml_map_annotations_v2&);
    // Copied native map-local cursor position. Queue an opted-in guest event;
    // never mutate the native six-slot database. False retains stock handling.
    bool placement_action(const std::array<float,2>& point,bool overflow) noexcept;
    // UI-authenticated hover lease, scoped to one visible editable world item.
    // True consumes native X and queues/coalesces its exact DELETE event.
    bool hovered_annotation_action(const std::array<float,2>& cursor) noexcept;
    int annotations_publish_v3(uint64_t owner,const crml_map_annotations_v3&);
    int annotations_next_v2(uint64_t owner,crml_map_annotation_event_v2&);
    int annotations_status(uint64_t owner,crml_map_annotation_status&);
    int annotations_next(uint64_t owner,crml_map_annotation_event&);
    int annotations_hide(uint64_t owner);
    int poll(uint64_t page,uint64_t sequence,std::vector<Surface>& output);
    int exchange(std::string_view url,std::string& output) noexcept;
    // Bounded renderer/host counters only; no game text, paths or coordinates.
    std::string diagnostics();
    // Internal renderer focus lease; never exposed as arbitrary guest input control.
    void enable_map_editor_input(bool enabled);
    bool map_editor_input_active() noexcept;
private:
    int annotation_exchange(std::string_view url,bool typed=false);
    int annotations_publish_copied(uint64_t owner,const crml_map_annotations_v2&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ModDrawing& process_drawing();
}
