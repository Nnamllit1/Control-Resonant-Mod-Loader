(module
  (import "crml_v1" "log" (func $log (param i32 i32 i32)))
  (import "crml_v1" "noclip_poll" (func $poll (param f32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "Experimental noclip: unavailable")
  (data (i32.const 64) "Experimental noclip: OFF (F6 to toggle)")
  (data (i32.const 128) "Experimental noclip: ON (F6 or Esc to exit)")
  (data (i32.const 192) "Noclip is owned by another mod")
  (global $previous (mut i32) (i32.const -99))
  (func (export "crml_abi_version") (result i32) i32.const 1)
  (func (export "crml_init"))
  (func (export "crml_tick") (param f32) (local $status i32)
    f32.const 5
    call $poll
    local.tee $status
    global.get $previous
    i32.ne
    if
      local.get $status
      global.set $previous
      local.get $status
      i32.const 1
      i32.eq
      if
        i32.const 1 i32.const 128 i32.const 43 call $log
      else
        local.get $status i32.eqz
        if
          i32.const 1 i32.const 64 i32.const 39 call $log
        else
          local.get $status i32.const -2 i32.eq
          if
            i32.const 2 i32.const 192 i32.const 30 call $log
          else
            i32.const 2 i32.const 0 i32.const 32 call $log
          end
        end
      end
    end)
  ;; The native owner cleanup also runs if this mod traps or cannot shut down.
  (func (export "crml_shutdown")))
