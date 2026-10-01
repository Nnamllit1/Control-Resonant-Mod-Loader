#pragma once
#include <stdint.h>

#define CRML_MEDIA_STATE_VERSION 1u
#define CRML_MEDIA_STATE_SIZE 288u
#define CRML_MEDIA_NAME_CAPACITY 256u
#define CRML_MEDIA_ACTIVE 1u
#define CRML_MEDIA_SKIPPABLE 2u
#define CRML_MEDIA_ENGINE_NAME 4u
#define CRML_MEDIA_MAPPED_NAME 8u

// Copied media observation. ENGINE_NAME identifies an actual engine asset path;
// it is never a friendly label synthesized by the loader. MAPPED_NAME instead
// identifies an engine asset reference associated with a reviewed adapter,
// rather than the observed instance's own name. Generation is opaque.
typedef struct crml_media_state {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t elapsed_ms;
    uint64_t generation;
    uint32_t name_length;
    uint32_t reserved;
    char name[CRML_MEDIA_NAME_CAPACITY]; // UTF-8 bytes, excluding the trailing NUL.
} crml_media_state;

#ifdef __cplusplus
static_assert(sizeof(crml_media_state) == CRML_MEDIA_STATE_SIZE);
#endif
