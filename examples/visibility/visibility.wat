(module
  (import "crml_v1" "input_buttons" (func $buttons (result i32)))
  (import "crml_v1" "visibility_set" (func $hide (param i32) (result i32)))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init"))
  ;; The mod chooses the key and decides whether to request hiding.
  ;; Change the mask to 2 for F8; no native rebuild is needed.
  (func (export "crml_tick") (param f32)
    (drop (call $hide (i32.ne (i32.and (call $buttons) (i32.const 1)) (i32.const 0)))))
  (func (export "crml_shutdown") (drop (call $hide (i32.const 0)))))
