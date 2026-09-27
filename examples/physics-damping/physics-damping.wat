(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "input_buttons" (func $input (result i32)))
  (import "crml_v1" "physics_select" (func $select (result i32)))
  (import "crml_v1" "physics_target" (func $target (result i64)))
  (import "crml_v1" "physics_apply" (func $apply (param i64 f32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "Wasm physics: F7 select, F8 damping, F11 restore.")
  (global $previous (mut i32) (i32.const 0))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init")
    i32.const 1 i32.const 0 i32.const 49 call $log)
  (func (export "crml_tick") (param f32)
    (local $buttons i32) (local $edges i32) (local $handle i64)
    call $input local.tee $buttons
    global.get $previous i32.const -1 i32.xor i32.and local.set $edges
    local.get $buttons global.set $previous
    local.get $edges i32.const 1 i32.and
    if call $select drop end
    local.get $edges i32.const 2 i32.and
    if
      call $target local.tee $handle i64.eqz
      if
      else local.get $handle f32.const 8 i32.const 5000 call $apply drop
      end
    end)
  ;; Native owner cleanup runs even without a guest shutdown export.
)
