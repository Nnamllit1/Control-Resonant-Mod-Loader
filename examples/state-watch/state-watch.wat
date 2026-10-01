(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "player_read" (func $player (param i32 i32) (result i32)))
  (import "crml_v1" "camera_read" (func $camera (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "State watch: player position available.")
  (data (i32.const 64) "State watch: player position unavailable.")
  (data (i32.const 128) "State watch: moved at least five world units.")
  (data (i32.const 192) "State watch: selected camera changed.")
  (data (i32.const 256) "State watch: camera unavailable.")
  (global $elapsed (mut f32) (f32.const 0))
  (global $player_ready (mut i32) (i32.const -1))
  (global $camera_ready (mut i32) (i32.const -1))
  (global $generation (mut i64) (i64.const 0))
  (global $camera_generation (mut i64) (i64.const 0))
  (global $mode (mut i32) (i32.const -1))
  (global $x (mut f32) (f32.const 0))
  (global $y (mut f32) (f32.const 0))
  (global $z (mut f32) (f32.const 0))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init"))
  (func $anchor
    i32.const 1040 f32.load global.set $x
    i32.const 1044 f32.load global.set $y
    i32.const 1048 f32.load global.set $z)
  (func (export "crml_tick") (param $dt f32)
    (local $ready i32) (local $dx f32) (local $dy f32) (local $dz f32)
    global.get $elapsed local.get $dt f32.add global.set $elapsed
    global.get $elapsed f32.const 0.25 f32.lt if return end
    f32.const 0 global.set $elapsed
    i32.const 1024 i32.const 32 call $player i32.const 1 i32.eq local.set $ready
    local.get $ready global.get $player_ready i32.ne
    if
      local.get $ready if i32.const 1 i32.const 0 i32.const 39 call $log else i32.const 1 i32.const 64 i32.const 41 call $log end
      ;; Reset the anchor after every unavailable interval.
      i64.const 0 global.set $generation
    end
    local.get $ready global.set $player_ready
    local.get $ready
    if
      i32.const 1032 i64.load global.get $generation i64.ne
      if i32.const 1032 i64.load global.set $generation call $anchor
      else
        i32.const 1040 f32.load global.get $x f32.sub local.set $dx
        i32.const 1044 f32.load global.get $y f32.sub local.set $dy
        i32.const 1048 f32.load global.get $z f32.sub local.set $dz
        local.get $dx local.get $dx f32.mul
        local.get $dy local.get $dy f32.mul f32.add
        local.get $dz local.get $dz f32.mul f32.add
        f32.const 25 f32.ge if i32.const 1 i32.const 128 i32.const 45 call $log call $anchor end
      end
    end
    i32.const 1088 i32.const 80 call $camera i32.const 1 i32.eq local.set $ready
    local.get $ready
    if
      global.get $camera_ready i32.const 1 i32.ne
      i32.const 1096 i64.load global.get $camera_generation i64.ne i32.or
      i32.const 1104 i32.load global.get $mode i32.ne i32.or
      if i32.const 1 i32.const 192 i32.const 37 call $log end
      i32.const 1096 i64.load global.set $camera_generation
      i32.const 1104 i32.load global.set $mode
    else
      global.get $camera_ready i32.eqz i32.eqz if i32.const 1 i32.const 256 i32.const 32 call $log end
    end
    local.get $ready global.set $camera_ready)
)
