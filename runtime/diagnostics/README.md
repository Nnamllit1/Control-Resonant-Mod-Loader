# Engine diagnostics

Normal C++ source files for engine-phase observation, fall/recovery traces, entity inspection and camera/physics snapshots. CMake compiles these into the existing runtime and native test targets. Their headers are included through `diagnostics/` from the runtime include directory.

Some observation helpers also support gameplay services. Keep their generation checks and scheduling contracts intact when changing them. Conditional synthetic checks remain guarded by their existing `CRML_*_TESTING` definitions; test executable entry points live in `tests/`.

The engine recorder also supports a focused camera capture. It uses only camera hooks and can run beside the shared gameplay services; the full engine recorder retains its isolated startup mode.
