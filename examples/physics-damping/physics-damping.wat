(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "input_actions" (func $input (result i32)))
  (import "crml_v1" "physics_select_near" (func $select (param f32 f32 f32 f32) (result i32)))
  (import "crml_v1" "physics_target" (func $target (result i64)))
  (import "crml_v1" "physics_status" (func $status (result i32)))
  (import "crml_v1" "physics_apply" (func $apply (param i64 f32 i32) (result i32)))
  (import "crml_v1" "physics_read" (func $read (param i64 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "Physics: Home select, End damping, F11 restore.")
  (global $previous (mut i32) (i32.const 0))
  (global $previous_status (mut i32) (i32.const -100))
  (global $retained_target (mut i64) (i64.const 0))
  (global $sampled_match_logged (mut i32) (i32.const 0))
  (global $damping f32 (f32.const 8))
  (global $duration_ms i32 (i32.const 5000))
  (data (i32.const 64) "Physics: prop selected.")
  (data (i32.const 96) "Physics: damping active.")
  (data (i32.const 128) "Physics: no selected prop.")
  (data (i32.const 192) "Physics: search queued.")
  (data (i32.const 256) "Physics: search unavailable or busy.")
  (data (i32.const 320) "Physics: damping queued.")
  (data (i32.const 384) "Physics: damping request rejected.")
  (data (i32.const 448) "Physics: select a prop first; wait for selection.")
  (data (i32.const 512) "Physics: sampled damping matches request.")
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init")
    i32.const 1 i32.const 0 i32.const 47 call $log)
  (func (export "crml_tick") (param f32)
    (local $buttons i32) (local $edges i32) (local $handle i64) (local $state i32) (local $read_status i32)
    call $input local.tee $buttons
    global.get $previous i32.const -1 i32.xor i32.and local.set $edges
    local.get $buttons global.set $previous
    local.get $edges i32.const 1 i32.and
    if
      ;; Mod policy: world-aligned player-relative offset, then search radius.
      f32.const 0 f32.const 0 f32.const 0 f32.const 2 call $select i32.eqz
      if i32.const 1 i32.const 192 i32.const 23 call $log
      else i32.const 2 i32.const 256 i32.const 36 call $log end
    end
    local.get $edges i32.const 2 i32.and
    if
      call $target local.tee $handle i64.eqz
      if
        i32.const 2 i32.const 448 i32.const 49 call $log
      else
        ;; Mod policy: linear damping and duration, not a native preset.
        local.get $handle global.get $damping global.get $duration_ms call $apply i32.eqz
        if
          local.get $handle global.set $retained_target
          i32.const 0 global.set $sampled_match_logged
          i32.const 1 i32.const 320 i32.const 24 call $log
        else i32.const 2 i32.const 384 i32.const 34 call $log end
      end
    end
    ;; Keep the token while active; the target getter then returns zero.
    ;; Snapshot occupies bytes 1024..1055, separate from all log strings.
    global.get $retained_target i64.eqz i32.eqz
    if
      global.get $retained_target i32.const 1024 i32.const 32 call $read local.set $read_status
      local.get $read_status i32.const -3 i32.eq
      if i64.const 0 global.set $retained_target
      else
        local.get $read_status i32.const 1 i32.eq
        i32.const 1024 i32.load i32.const 1 i32.eq i32.and
        global.get $sampled_match_logged i32.eqz i32.and
        i32.const 1032 i32.load i32.const 1 i32.and i32.eqz i32.eqz i32.and
        i32.const 1040 f32.load global.get $damping f32.eq i32.and
        if
          i32.const 1 i32.const 512 i32.const 41 call $log
          i32.const 1 global.set $sampled_match_logged
        end
      end
    end
    call $status local.set $state
    local.get $state i32.eqz
    local.get $state i32.const 6 i32.eq i32.or
    local.get $state i32.const 7 i32.eq i32.or
    local.get $state i32.const 8 i32.eq i32.or
    local.get $state i32.const 9 i32.eq i32.or
    if i64.const 0 global.set $retained_target end
    local.get $state global.get $previous_status i32.ne
    if
      local.get $state global.set $previous_status
      local.get $state i32.const 3 i32.eq
      if i32.const 1 i32.const 64 i32.const 23 call $log end
      local.get $state i32.const 4 i32.eq
      if i32.const 1 i32.const 96 i32.const 24 call $log end
      local.get $state i32.eqz
      if i32.const 1 i32.const 128 i32.const 26 call $log end
    end)
  ;; Native owner cleanup runs even without a guest shutdown export.
)
