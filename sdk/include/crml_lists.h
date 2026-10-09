#ifndef CRML_LISTS_H
#define CRML_LISTS_H
#include <stdint.h>

#define CRML_LIST_VERSION 1u
#define CRML_LIST_ROWS_MAX 32u
#define CRML_LIST_ROW_ENABLED 1u
#define CRML_LIST_ROW_SELECTED 2u
#define CRML_LIST_EVENTS_MAX 16u

// Stable guest-defined nonzero IDs, unique in this page. At most one selected
// row. Labels are required; detail may be empty. NUL-terminated single-line
// UTF-8; no C0/C1 controls or U+2028/U+2029. Zero-initialize descriptors.
typedef struct crml_list_row {
    uint64_t id;
    uint32_t flags, reserved;
    char label[96], detail[192];
} crml_list_row;

// Whole copied page, including an empty page. No game/content identity is implied.
typedef struct crml_list_page {
    uint32_t version, size, row_count;
    float font_scale; // Finite 0.75..1.5, applied to list text only.
    char title[96];
    crml_list_row rows[CRML_LIST_ROWS_MAX];
} crml_list_page;

// Destructive FIFO read. Sequence and revision are transient host identities.
// Accepted events retain the revision at activation even after a page replacement.
// flags is zero in version 1. Non-successful reads clear the whole output.
typedef struct crml_list_event {
    uint32_t version, flags;
    uint64_t sequence, revision, row_id;
} crml_list_event;

#if defined(__cplusplus)
static_assert(sizeof(crml_list_row)==304);
static_assert(sizeof(crml_list_page)==9840);
static_assert(sizeof(crml_list_event)==32);
#endif
#endif
