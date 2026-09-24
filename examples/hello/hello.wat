(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (memory (export "memory") 1 1)
  (data (i32.const 0) "Hello from a sandboxed CONTROL Resonant mod!")
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init")
    i32.const 1 i32.const 0 i32.const 44 call $log)
  (func (export "crml_tick") (param f32))
  (func (export "crml_shutdown")))
