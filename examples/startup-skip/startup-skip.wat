(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "ui_read" (func $read (param i32 i32) (result i32)))
  (import "crml_v1" "ui_activate" (func $activate (param i64 i32) (result i32)))
  (import "crml_v1" "media_read" (func $media_read (param i32 i32) (result i32)))
  (import "crml_v1" "media_skip" (func $media_skip (param i64) (result i32)))
  (import "crml_v1" "ui_present" (func $present (param i64 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "Startup skip: ready.")
  (data (i32.const 64) "Startup skip: continue requested.")
  (data (i32.const 128) "Startup skip: media skip requested.")
  (data (i32.const 192) "splash")
  (data (i32.const 512) "textures/videos/uiresources/splash/boot.tex")
  (global $generation (mut i64) (i64.const 0))
  (global $screen (mut i32) (i32.const 0))
  (global $attempts (mut i32) (i32.const 0))
  (global $observed (mut i32) (i32.const 0))
  (global $stable (mut f32) (f32.const 0))
  (global $retry (mut f32) (f32.const 0))
  (global $presentation_generation (mut i64) (i64.const 0))
  (global $presentation_desired (mut i32) (i32.const 0))
  (global $presentation_retry (mut f32) (f32.const 0))
  (global $media_generation (mut i64) (i64.const 0))
  (global $media_attempts (mut i32) (i32.const 0))
  (global $media_observed (mut i32) (i32.const 0))
  (global $media_retry (mut f32) (f32.const 0))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init") i32.const 1 i32.const 0 i32.const 20 call $log)
  (func (export "crml_shutdown"))
  (func $reset_timing
    f32.const 0 global.set $stable
    f32.const 0 global.set $retry)
  (func $boot_asset (result i32) (local $i i32) (local $c i32)
    i32.const 1048 i32.load i32.const 43 i32.ne
    i32.const 1099 i32.load8_u i32.or
    if i32.const 0 return end
    (block $done (loop $compare
      local.get $i i32.const 43 i32.ge_u br_if $done
      i32.const 1056 local.get $i i32.add i32.load8_u local.set $c
      local.get $c i32.const 92 i32.eq if i32.const 47 local.set $c end
      i32.const 512 local.get $i i32.add i32.load8_u local.get $c i32.ne
      if i32.const 0 return end
      local.get $i i32.const 1 i32.add local.set $i br $compare))
    i32.const 1)
  (func $media_tick (param $dt f32)
    ;; Media snapshot at 1024, including its bounded engine asset name.
    i32.const 1024 i32.const 288 call $media_read i32.const 1 i32.ne
    if i32.const 0 global.set $media_observed f32.const 0 global.set $media_retry return end
    i32.const 1024 i32.load i32.const 288 i32.ne
    i32.const 1028 i32.load i32.const 1 i32.ne i32.or
    i32.const 1040 i64.load i64.eqz i32.or
    if i32.const 0 global.set $media_observed f32.const 0 global.set $media_retry return end
    i32.const 1040 i64.load global.get $media_generation i64.ne
    if
      i32.const 1040 i64.load global.set $media_generation
      i32.const 0 global.set $media_attempts
      i32.const 1 global.set $media_observed
      f32.const 0 global.set $media_retry return
    end
    global.get $media_observed i32.eqz
    if i32.const 1 global.set $media_observed f32.const 0 global.set $media_retry return end
    global.get $media_retry local.get $dt f32.add f32.const 0.25 f32.min global.set $media_retry
    global.get $media_attempts i32.const 10 i32.ge_u
    global.get $media_retry f32.const 0.25 f32.lt i32.or
    i32.const 1036 i32.load i32.const 2000 i32.le_u i32.or
    i32.const 1032 i32.load i32.const 3 i32.and i32.const 3 i32.ne i32.or
    i32.const 1032 i32.load i32.const 12 i32.and i32.eqz i32.or
    if return end
    call $boot_asset i32.eqz if return end
    global.get $media_attempts i32.const 1 i32.add global.set $media_attempts
    f32.const 0 global.set $media_retry
    global.get $media_generation call $media_skip i32.eqz
    if i32.const 1 i32.const 128 i32.const 35 call $log end)
  (func $presentation_tick (param $dt f32) (local $eligible i32) (local $changed i32)
    i32.const 264 i32.load i32.const 1 i32.eq
    i32.const 264 i32.load i32.const 5 i32.eq i32.or
    i32.const 264 i32.load i32.const 8 i32.eq i32.or local.set $eligible
    i32.const 272 i64.load global.get $presentation_generation i64.ne local.set $changed
    i32.const 272 i64.load global.set $presentation_generation
    global.get $presentation_retry local.get $dt f32.add f32.const 0.25 f32.min global.set $presentation_retry
    local.get $eligible
    if
      global.get $presentation_desired i32.eqz local.get $changed i32.or
      global.get $presentation_retry f32.const 0.25 f32.ge i32.or
      if
        i32.const 1 global.set $presentation_desired
        f32.const 0 global.set $presentation_retry
        i32.const 272 i64.load i32.const 2 i32.const 192 i32.const 6 i32.const 1 i32.const 750 call $present drop
      end
    else
      global.get $presentation_desired
      local.get $changed global.get $presentation_retry f32.const 0.25 f32.ge i32.or i32.and
      if
        f32.const 0 global.set $presentation_retry
        i32.const 272 i64.load i32.const 2 i32.const 192 i32.const 6 i32.const 0 i32.const 0 call $present i32.eqz
        if i32.const 0 global.set $presentation_desired end
      end
    end)
  (func (export "crml_tick") (param $dt f32)
    local.get $dt call $media_tick
    ;; Snapshot at 256: size, version, screen, actions, generation, age, reserved.
    i32.const 256 i32.const 32 call $read i32.const 1 i32.ne
    if i32.const 0 global.set $observed call $reset_timing return end
    i32.const 256 i32.load i32.const 32 i32.ne
    i32.const 260 i32.load i32.const 1 i32.ne i32.or
    i32.const 272 i64.load i64.eqz i32.or
    if i32.const 0 global.set $observed call $reset_timing return end
    local.get $dt call $presentation_tick
    i32.const 272 i64.load global.get $generation i64.ne
    i32.const 264 i32.load global.get $screen i32.ne i32.or
    if
      i32.const 272 i64.load global.set $generation
      i32.const 264 i32.load global.set $screen
      i32.const 0 global.set $attempts
      i32.const 1 global.set $observed
      call $reset_timing
      return
    end
    global.get $observed i32.eqz
    if i32.const 1 global.set $observed call $reset_timing return end
    global.get $stable local.get $dt f32.add f32.const 0.1 f32.min global.set $stable
    global.get $retry local.get $dt f32.add f32.const 0.25 f32.min global.set $retry
    global.get $attempts i32.const 10 i32.ge_u
    global.get $stable f32.const 0.1 f32.lt i32.or
    global.get $retry f32.const 0.25 f32.lt i32.or
    i32.const 268 i32.load i32.const 1 i32.and i32.eqz i32.or
    if return end
    ;; Deliberately exclude legal/EULA/privacy, calibration/setup and main menu.
    global.get $screen i32.const 1 i32.eq
    global.get $screen i32.const 5 i32.eq i32.or
    global.get $screen i32.const 8 i32.eq i32.or i32.eqz
    if return end
    global.get $attempts i32.const 1 i32.add global.set $attempts
    f32.const 0 global.set $retry
    global.get $generation i32.const 1 call $activate i32.eqz
    if i32.const 1 i32.const 64 i32.const 33 call $log end)
)
