#pragma once
#include <stdint.h>

// Guest SDK, compiled to wasm32 (no WASI or native DLL target).
#if !defined(__wasm32__)
#error "CRML mods must target wasm32"
#endif
#define CRML_EXPORT(name) __attribute__((export_name(name)))
#ifdef __cplusplus
extern "C" {
#endif
// Requires capabilities=log. UTF-8 bytes in guest memory, at most 4096 per call.
__attribute__((import_module("crml_v1"), import_name("log")))
void crml_log(int32_t level, const char* text, uint32_t length);
CRML_EXPORT("crml_abi_version") uint32_t crml_abi_version(void);
CRML_EXPORT("crml_init") void crml_init(void);
// Optional; worker heartbeat, NOT a game frame or game-thread callback.
CRML_EXPORT("crml_tick") void crml_tick(float elapsed_seconds);
CRML_EXPORT("crml_shutdown") void crml_shutdown(void);
#ifdef __cplusplus
}
#endif
