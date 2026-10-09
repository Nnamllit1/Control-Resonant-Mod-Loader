#ifndef CRML_TUTORIAL_H
#define CRML_TUTORIAL_H
#include <stdint.h>
#define CRML_TUTORIAL_HINT 0u
#define CRML_TUTORIAL_PANEL 1u
#define CRML_TUTORIAL_PROMPT 2u
#define CRML_TUTORIAL_QUEUED 1
#define CRML_TUTORIAL_PRESENTED 2
#define CRML_TUTORIAL_DISMISSED 3
#define CRML_TUTORIAL_CANCELLED 4
#define CRML_TUTORIAL_UNAVAILABLE 5
#define CRML_TUTORIAL_FAILED 6
#define CRML_TUTORIAL_CANCELLING 7
#define CRML_TUTORIAL_TITLE_MAX 128u
#define CRML_TUTORIAL_BODY_MAX 1024u
#define CRML_TUTORIAL_IMAGE_URL_MAX 256u
#define CRML_TUTORIAL_PAGE_VERSION 1u
#define CRML_TUTORIAL_PAGE_VERSION_OPTIONS 2u
#define CRML_TUTORIAL_PAGE_SIZE 1444u
#define CRML_TUTORIAL_OPTION_NATIVE_DISMISS 1u
#define CRML_TUTORIAL_OPTION_PROGRESS 2u
#define CRML_TUTORIAL_IMAGE_ABOVE 0u
#define CRML_TUTORIAL_IMAGE_BELOW 1u
#define CRML_TUTORIAL_ALIGN_CENTER 0u
#define CRML_TUTORIAL_ALIGN_LEFT 1u
#define CRML_TUTORIAL_ALIGN_RIGHT 2u

// Zero values select above/center/100 percent/18 vh/1 vh. Nonzero width is
// 1..100 percent, height 1..32 vh, gap 1..4 vh. Layout does not move the native
// heading or Continue control. No text wrapping around the image.
typedef struct crml_tutorial_image_layout {
    uint32_t position, alignment, width_percent, max_height_vh, gap_vh;
} crml_tutorial_image_layout;

// Zero-initialize, set version, then fill NUL-terminated UTF-8 strings.
// Fixed arrays contain guest bytes, never native pointers. Empty image means
// text only. Version 1 requires reserved zero. For version 2, reserved holds
// CRML_TUTORIAL_OPTION_* bits for CRML_TUTORIAL_PROMPT only. The size argument
// must equal sizeof(page); both versions have the same fixed ABI.
typedef struct crml_tutorial_page {
    uint32_t version, kind, duration_ms;
    crml_tutorial_image_layout image;
    char title[CRML_TUTORIAL_TITLE_MAX+1];
    char body[CRML_TUTORIAL_BODY_MAX+1];
    char image_url[CRML_TUTORIAL_IMAGE_URL_MAX+1];
    uint8_t reserved;
} crml_tutorial_page;
#ifdef __cplusplus
static_assert(sizeof(crml_tutorial_page)==CRML_TUTORIAL_PAGE_SIZE);
#else
_Static_assert(sizeof(crml_tutorial_page)==CRML_TUTORIAL_PAGE_SIZE,"Tutorial descriptor ABI");
#endif
#endif
