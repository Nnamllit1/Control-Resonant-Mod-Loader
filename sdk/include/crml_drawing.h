#ifndef CRML_DRAWING_H
#define CRML_DRAWING_H
#include <stdint.h>

#define CRML_DRAWING_VERSION 1u
#define CRML_DRAWING_SEGMENTS_MAX 128u
#define CRML_DRAWING_LABELS_MAX 32u
#define CRML_DRAWING_LABEL_BYTES 64u
#define CRML_DRAWING_VISIBLE_MAX 4u
// All coordinates are normalized, with (0,0) at top-left and positive Y down.
// A surface rectangle is relative to the viewport. Primitive positions are
// relative to that rectangle. This is a schematic, not world-space projection.
// Colors use 0xRRGGBBAA. Width and font size are in viewport-height percent (vh).
typedef struct crml_drawing_segment {
    float x1, y1, x2, y2, width_vh;
    uint32_t rgba;
} crml_drawing_segment;
typedef struct crml_drawing_label {
    float x, y, font_vh;
    uint32_t rgba;
    char text[CRML_DRAWING_LABEL_BYTES]; // NUL-terminated, single-line UTF-8.
} crml_drawing_label;
typedef struct crml_drawing_frame {
    uint32_t version, size;
    uint32_t lifetime_ms; // 100..1000 host-monotonic ms; renew to remain visible.
    uint32_t segment_count, label_count;
    uint32_t reserved[3]; // Must be zero.
    float x, y, width, height; // Each extent >=0.05; entire rectangle within [0,1].
    crml_drawing_segment segments[CRML_DRAWING_SEGMENTS_MAX];
    crml_drawing_label labels[CRML_DRAWING_LABELS_MAX];
} crml_drawing_frame;

// Native-map drawing is a separate contract; schematic ABI 1 is unchanged.
#define CRML_MAP_VERSION 1u
#define CRML_MAP_FULL 1u
#define CRML_MAP_SONAR 2u // Native HUD minimap; requires runtime dev.4 or newer.

// Additive dev.5 contract. Interactive annotations are separate from passive
// drawing ABI 1. The guest owns IDs/data and applies queued edits explicitly.
#define CRML_MAP_ANNOTATIONS_MAX 24u
#define CRML_MAP_ANNOTATIONS_V2_MAX 128u
#define CRML_MAP_ANNOTATIONS_V3_MAX 128u
#define CRML_MAP_NATIVE_ATTACHMENTS_MAX 6u
#define CRML_MAP_ANNOTATIONS_PLACE_ACTION 4u // v2: receive overflow from the native map placement action.
#define CRML_MAP_ANNOTATIONS_CREATE 1u
#define CRML_MAP_ANNOTATIONS_NATIVE_MARKERS 2u // dev.6: offer metadata editing on native X markers.
#define CRML_MAP_ANNOTATION_EDITABLE 1u
#define CRML_MAP_ANNOTATION_NATIVE_MARKER 2u // id = native number 1..6; position = map-local pixels (x,y,0).
#define CRML_MAP_ANNOTATION_CREATE 1u
#define CRML_MAP_ANNOTATION_UPDATE 2u
#define CRML_MAP_ANNOTATION_DELETE 3u
#define CRML_MAP_ANNOTATION_ATTACH 4u // Native marker metadata upsert; never creates/deletes a game marker.
#define CRML_MAP_ANNOTATION_TARGET_WORLD 1u
#define CRML_MAP_ANNOTATION_TARGET_NATIVE 2u
typedef struct crml_map_annotation {
    uint64_t id;
    float position[3];
    uint32_t rgba, flags;
    float distance; // Native attachments require 0 (no verified world position). Otherwise 0: unrestricted; otherwise maximum world-space distance.
    char symbol[8]; // One ASCII letter/digit or *, +, !, ?.
    char name[64], description[128]; // Single-line UTF-8; description may be empty.
} crml_map_annotation;
typedef struct crml_map_annotations {
    uint32_t version, size, count, flags;
    uint64_t context, revision; // Guest data revision; changes on edits, not camera motion.
    float origin[3], up[3]; // Creation plane and distance reference; no floor identity implied.
    crml_map_annotation items[CRML_MAP_ANNOTATIONS_MAX];
} crml_map_annotations;
// Additive dev.8 descriptor. The v1 structure and import retain their sizes.
typedef struct crml_map_annotations_v2 {
    uint32_t version, size, count, flags;
    uint64_t context, revision;
    float origin[3], up[3];
    crml_map_annotation items[CRML_MAP_ANNOTATIONS_V2_MAX];
} crml_map_annotations_v2;
// Additive dev.10 descriptor. World IDs and native slots have separate namespaces.
// A native attachment is presentation metadata for an existing stock X marker;
// map_pixels are map-local pixels and do not imply a world position or distance.
typedef struct crml_map_world_annotation {
    uint64_t id; // Nonzero published guest ID; zero only in a CREATE event.
    float world_position[3];
    uint32_t rgba, flags; // Only CRML_MAP_ANNOTATION_EDITABLE is valid.
    float distance; // 0: unrestricted; otherwise maximum world-space distance.
    char symbol[8]; // One ASCII letter/digit or *, +, !, ?.
    char name[64], description[128]; // Single-line UTF-8; description may be empty.
} crml_map_world_annotation;
typedef struct crml_map_native_attachment {
    uint32_t native_slot; // Stock marker number 1..6, never a guest world ID.
    uint32_t flags; // Only CRML_MAP_ANNOTATION_EDITABLE is valid.
    uint32_t rgba;
    uint32_t reserved; // Must be zero.
    float map_pixels[2]; // Native map-local pixel x,y; no world/distance claim.
    char symbol[8];
    char name[64], description[128];
} crml_map_native_attachment;
typedef struct crml_map_annotations_v3 {
    uint32_t version, size; // version=3, size=sizeof(crml_map_annotations_v3).
    uint32_t count, attachment_count; // Count sum <=128; attachments <=6.
    uint32_t flags, reserved; // CREATE/NATIVE_MARKERS/PLACE_ACTION; reserved=0.
    uint64_t context, revision; // Fresh full-map context; nonzero data revision.
    float origin[3], up[3];
    crml_map_world_annotation items[CRML_MAP_ANNOTATIONS_V3_MAX];
    crml_map_native_attachment attachments[CRML_MAP_NATIVE_ATTACHMENTS_MAX];
} crml_map_annotations_v3;
typedef struct crml_map_annotation_event {
    uint32_t version, action;
    uint64_t revision;
    crml_map_annotation item; // Create has id=0 and a point on the supplied plane.
} crml_map_annotation_event;
typedef struct crml_map_annotation_event_v2 {
    uint32_t version, action; // version=2; CREATE/UPDATE/DELETE/ATTACH.
    uint64_t revision, context;
    uint32_t target, reserved; // WORLD or NATIVE; reserved must be zero.
    crml_map_world_annotation world; // Active for WORLD; CREATE has id=0.
    crml_map_native_attachment attachment; // Active for NATIVE; slot remains 1..6.
    // The inactive member is zeroed by the host.
} crml_map_annotation_event_v2;
typedef struct crml_map_annotation_status {
    uint32_t version, size; // version=1, size=sizeof(crml_map_annotation_status).
    uint32_t requested, granted; // Only NATIVE_MARKERS/PLACE_ACTION bits.
    uint64_t context, revision; // Current owner publication, not a save identity.
} crml_map_annotation_status;
typedef struct crml_map_state {
    uint32_t version, size, target, reserved;
    uint64_t context; // Ephemeral destination stream/geometry token; never a save/campaign ID.
} crml_map_state;
// Copied affine full-map projection. Rows map world XYZ to normalized map XY:
// uv[row] = sum(world_to_map[row*4+j]*world[j], j=0..2) + row[3].
// Geometry may be shared by saves; these values are NOT a campaign identity.
typedef struct crml_map_projection {
    uint32_t version, size;
    uint64_t context;
    float world_to_map[8], rect[4]; // rect in native map-local pixels.
} crml_map_projection;

typedef struct crml_map_segment {
    float from[3], to[3], width_vh;
    uint32_t rgba;
} crml_map_segment;
typedef struct crml_map_label {
    float position[3], font_vh;
    uint32_t rgba;
    char text[CRML_DRAWING_LABEL_BYTES];
} crml_map_label;
typedef struct crml_map_frame {
    uint32_t version, size, target, lifetime_ms;
    uint64_t context;
    uint32_t segment_count, label_count;
    crml_map_segment segments[CRML_DRAWING_SEGMENTS_MAX];
    crml_map_label labels[CRML_DRAWING_LABELS_MAX];
} crml_map_frame;
#if defined(__cplusplus)
static_assert(sizeof(crml_map_annotation)==232 && sizeof(crml_map_annotations)==5624 && sizeof(crml_map_annotation_event)==248);
static_assert(sizeof(crml_map_annotations_v2)==29752);
static_assert(sizeof(crml_map_world_annotation)==232 && sizeof(crml_map_native_attachment)==224);
static_assert(sizeof(crml_map_annotations_v3)==31104 && sizeof(crml_map_annotation_event_v2)==488);
static_assert(sizeof(crml_map_annotation_status)==32);
static_assert(sizeof(crml_drawing_segment)==24);
static_assert(sizeof(crml_drawing_label)==80);
static_assert(sizeof(crml_drawing_frame)==5680);
static_assert(sizeof(crml_map_projection)==64);
static_assert(sizeof(crml_map_state)==24 && sizeof(crml_map_segment)==32);
static_assert(sizeof(crml_map_label)==84 && sizeof(crml_map_frame)==6816);
#endif
#endif
