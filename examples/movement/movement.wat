(module
  (import "crml_v1" "input_motion" (func $input (result i32)))
  (import "crml_v1" "motion_camera" (func $camera (param i32) (result i32)))
  (import "crml_v1" "motion_set" (func $set (param i32 f32 f32 f32) (result i32)))
  (memory (export "memory") 1)
  (global $previous (mut i32) (i32.const 0))
  (global $enabled (mut i32) (i32.const 0))
  (func $axis (param $buttons i32) (param $positive i32) (param $negative i32) (result f32)
    local.get $buttons local.get $positive i32.and i32.const 0 i32.ne f32.convert_i32_s
    local.get $buttons local.get $negative i32.and i32.const 0 i32.ne f32.convert_i32_s
    f32.sub)
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init"))
  (func (export "crml_tick") (param f32)
    (local $buttons i32) (local $right f32) (local $forward f32)
    (local $x f32) (local $y f32) (local $z f32) (local $scale f32)
    call $input local.tee $buttons
    global.get $previous i32.const -1 i32.xor i32.and i32.const 1 i32.and
    if global.get $enabled i32.eqz global.set $enabled end
    local.get $buttons global.set $previous
    global.get $enabled
    if
      ;; D-A and W-S are relative to the camera heading; altitude is independent.
      local.get $buttons i32.const 16 i32.const 8 call $axis local.set $right
      local.get $buttons i32.const 2 i32.const 4 call $axis local.set $forward
      local.get $buttons i32.const 32 i32.const 64 call $axis local.set $y
      i32.const 0 call $camera i32.const 1 i32.eq
      if
        local.get $right i32.const 0 f32.load f32.mul
        local.get $forward i32.const 4 f32.load f32.mul f32.sub local.set $x
        local.get $right i32.const 4 f32.load f32.mul
        local.get $forward i32.const 0 f32.load f32.mul f32.add local.set $z
      end
      ;; Normalize diagonals and choose 5 or 15 units/s entirely in the guest.
      local.get $buttons i32.const 128 i32.and
      if (result f32) f32.const 15 else f32.const 5 end
      local.get $x local.get $x f32.mul local.get $y local.get $y f32.mul f32.add
      local.get $z local.get $z f32.mul f32.add f32.sqrt f32.const 1 f32.max
      f32.div local.set $scale
      local.get $x local.get $scale f32.mul local.set $x
      local.get $y local.get $scale f32.mul local.set $y
      local.get $z local.get $scale f32.mul local.set $z
    end
    global.get $enabled local.get $x local.get $y local.get $z call $set i32.const 0 i32.lt_s
    if i32.const 0 global.set $enabled end)
  (func (export "crml_shutdown")
    i32.const 0 f32.const 0 f32.const 0 f32.const 0 call $set drop))
