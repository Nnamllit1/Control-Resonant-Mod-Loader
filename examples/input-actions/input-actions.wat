(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "capabilities" (func $capabilities (result i32)))
  (import "crml_v1" "input_actions" (func $actions (result i32)))
  (memory (export "memory") 1 1)
  (data (i32.const 0) "Input actions ready")
  (data (i32.const 32) "Input actions unavailable")
  (data (i32.const 64) "Input action 0 pressed")
  (data (i32.const 96) "Input action 1 pressed")
  (global $previous (mut i32) (i32.const 0))
  (global $first (mut i32) (i32.const 1))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init")
    call $capabilities i32.const 128 i32.and
    if
      i32.const 1 i32.const 0 i32.const 19 call $log
    else
      i32.const 1 i32.const 32 i32.const 25 call $log
    end)
  (func (export "crml_tick") (param f32) (local $current i32) (local $pressed i32)
    call $actions local.set $current
    global.get $first i32.eqz
    if
      local.get $current global.get $previous i32.const -1 i32.xor i32.and local.set $pressed
    end
    local.get $current global.set $previous
    i32.const 0 global.set $first
    local.get $pressed i32.const 1 i32.and
    if i32.const 1 i32.const 64 i32.const 22 call $log end
    local.get $pressed i32.const 2 i32.and
    if i32.const 1 i32.const 96 i32.const 22 call $log end)
  (func (export "crml_shutdown")))
