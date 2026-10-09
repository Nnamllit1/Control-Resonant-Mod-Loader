# MinHook 1.3.4 decodes one instruction past the displaced prefix before
# emitting its jump back. HDE64 cannot decode AVX, even when that instruction
# stays untouched in the original function. Build a narrowly patched copy;
# keep the downloaded dependency and its license intact.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MINHOOK_ROOT}/src/trampoline.c")
file(READ "${MINHOOK_ROOT}/src/trampoline.c" CRML_MINHOOK_SOURCE)
set(CRML_MINHOOK_OLD "        if (hs.flags & F_ERROR)\n            return FALSE;")
set(CRML_MINHOOK_NEW "        if (hs.flags & F_ERROR)\n        {\n            // Unsupported instructions may remain at the jump-back target.\n            // Never relocate them or skip an unfinished internal branch.\n            if (oldPos < sizeof(JMP_REL) || pOldInst < jmpDest)\n                return FALSE;\n            hs.len = 0;\n        }")
string(FIND "${CRML_MINHOOK_SOURCE}" "${CRML_MINHOOK_OLD}" CRML_MINHOOK_PATCH_OFFSET)
if(CRML_MINHOOK_PATCH_OFFSET EQUAL -1)
  message(FATAL_ERROR "MinHook trampoline source changed; review the bounded jump-back fix.")
endif()
string(REPLACE "${CRML_MINHOOK_OLD}" "${CRML_MINHOOK_NEW}" CRML_MINHOOK_SOURCE "${CRML_MINHOOK_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/minhook")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/minhook/trampoline.c" CONTENT "${CRML_MINHOOK_SOURCE}" @ONLY)
set(CRML_MINHOOK_TRAMPOLINE "${CMAKE_CURRENT_BINARY_DIR}/generated/minhook/trampoline.c")
