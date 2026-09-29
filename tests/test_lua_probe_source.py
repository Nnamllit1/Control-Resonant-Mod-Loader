"""Check the self-authored event script against a standalone Luau mock host."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
VM = None


class SourceTests(unittest.TestCase):
    def test_initialization_rollback(self):
        if VM is None:
            self.skipTest("supply --vm for standalone Lua behavior checks")
        source = (ROOT / "examples/lua-probe/persistent_events.luau").read_text(encoding="utf-8")
        host = '''
local mode = MODE
local owner, handler = {}, nil
local added, removed, sent, nextHandle, currentHandle = 0, 0, 0, 0, nil
local env = {self = owner}
env._ENV = env
setmetatable(env, {__index = getfenv(0)})
env.nl_add_event_handler = function(target, name, receive)
    assert(target == owner and name == 'lua.crml_persistent_f90c7812_own_event')
    assert(handler == nil, 'previous registration must have been cleaned up')
    if mode == 'add_error' or (mode == 'retry_error' and added == 1) then error('add failed') end
    added += 1
    nextHandle += 1
    currentHandle, handler = nextHandle, receive
    return currentHandle
end
env.nl_remove_event_handler = function(handle)
    assert(handle == currentHandle and handler ~= nil, 'never retry retired handle')
    removed += 1
    if mode == 'remove_error' then error('remove failed') end
    if mode ~= 'stuck' then currentHandle, handler = nil, nil end
    if mode == 'remove_then_error' then error('failed after release') end
end
env.nl_send_custom_event = function(name, value)
    assert(name == 'crml_persistent_f90c7812_own_event')
    sent += 1
    if handler then handler(owner, value) end
end
local chunk = function(...)
SOURCE
end
setfenv(chunk, env)
if mode == 'missing_pcall' then env.pcall = false; assert(chunk() == nil and added == 0); return end
local callback = chunk(true, true, true)
assert(type(callback) == 'function' and added == 0 and removed == 0 and sent == 0)
-- This is also the retain-allocation failure boundary: construction owns no
-- engine resources, so dropping the unretained closure needs no engine cleanup.
if mode == 'unstarted_shutdown' then
    assert(callback(callback) == 0 and added == 0 and removed == 0)
    assert(callback() == -406 and added == 0)
    return
end
local first = callback()
if mode == 'remove_error' or mode == 'remove_then_error' then
    assert(first == -405 and removed == 1 and sent == 0)
    assert(callback(callback) == -405 and callback(callback) == -405 and removed == 1)
    assert(callback() == -406)
    return
end
if mode == 'stuck' then
    assert(first == -401 and added == 1 and removed == 1)
    assert(callback(callback) == -401 and removed == 1)
    return
end
if mode == 'add_error' then
    assert(first == -404 and added == 0 and removed == 0)
    assert(callback(callback) == 1)
    return
end
assert(first == -410 and added == 1 and removed == 1 and handler == nil and sent == 1)
if mode == 'shutdown_after_rollback' then
    assert(callback(callback) == 1 and removed == 1 and added == 1)
    assert(callback() == -406)
    return
end
local second = callback()
if mode == 'retry_error' then
    assert(second == -404 and added == 1 and removed == 1 and handler == nil)
    assert(callback(callback) == 2 and removed == 1)
    return
end
assert(second == 2 and callback() == 3 and added == 2 and removed == 1)
assert(callback(callback) == 3 and removed == 2 and handler == nil)
assert(callback(callback) == 3 and removed == 2, 'shutdown must be idempotent')
assert(callback() == -406 and added == 2, 'stopped controller must never register again')
'''
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "initialization.luau"
            for mode in ("success", "add_error", "retry_error", "remove_error", "remove_then_error",
                         "stuck", "unstarted_shutdown", "shutdown_after_rollback", "missing_pcall"):
                with self.subTest(mode=mode):
                    script.write_text(host.replace('MODE', repr(mode)).replace('SOURCE', source), encoding="utf-8")
                    result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_listener_error_cleanup(self):
        if VM is None:
            self.skipTest("supply --vm for standalone Lua behavior checks")
        source = (ROOT / "examples/lua-probe/persistent_events.luau").read_text(encoding="utf-8")
        host = '''
local mode = MODE
local owner, handler = {}, nil
local explicit, errors, sent = 0, 0, 0
local env = {self = owner}
env._ENV = env
setmetatable(env, {__index = getfenv(0)})
env.nl_add_event_handler = function(target, name, receive)
    assert(target == owner and name == 'lua.crml_persistent_f90c7812_own_event')
    handler = receive
    return 91
end
env.nl_remove_event_handler = function(handle)
    -- A record retired by the engine must never be removed a second time.
    assert(handle == 91 and handler ~= nil and errors == 0)
    explicit += 1
    handler = nil
end
env.nl_send_custom_event = function(name, value)
    assert(name == 'crml_persistent_f90c7812_own_event')
    sent += 1
    if handler then
        local ok, err = pcall(handler, owner, value)
        if not ok then
            assert(type(err) == 'string')
            errors += 1
            if mode ~= 'stuck' then handler = nil end
            if mode == 'propagated' then error(err) end
        end
    end
end
local chunk = function(...)
SOURCE
end
setfenv(chunk, env)
local callback = chunk(true, true)
assert(callback() == 1 and callback() == 2)
if mode == 'early_shutdown' then
    assert(callback(callback) == 2 and explicit == 1 and errors == 0)
    return
end
local ok, result = pcall(callback)
if mode == 'propagated' then
    assert(not ok and callback(callback) == 3 and explicit == 0 and errors == 1)
    return
end
assert(ok and result == 3 and errors == 1 and explicit == 0)
local fourth = callback()
if mode == 'stuck' then
    assert(fourth == -402 and callback(callback) == -401 and explicit == 0)
else
    assert(fourth == 4 and callback(callback) == 4 and handler == nil)
    assert(errors == 1 and explicit == 0 and sent == 5)
end
print('listener error cleanup passed')
'''
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "listener_error.luau"
            for mode in ("success", "stuck", "early_shutdown", "propagated"):
                with self.subTest(mode=mode):
                    script.write_text(host.replace('MODE', repr(mode)).replace('SOURCE', source), encoding="utf-8")
                    result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_persistent_events(self):
        if VM is None:
            self.skipTest("supply --vm for standalone Lua behavior checks")
        source = (ROOT / "examples/lua-probe/persistent_events.luau").read_text(encoding="utf-8")
        host = '''
local mode = MODE
local owner, handler = {}, nil
local added, removed, sent = 0, 0, 0
local env = {self = owner}
env._ENV = env
setmetatable(env, {__index = getfenv(0)})
env.nl_add_event_handler = function(target, name, receive)
    assert(target == owner and name == 'lua.crml_persistent_f90c7812_own_event')
    assert(getfenv(receive).self == owner)
    added += 1
    handler = receive
    return 91
end
env.nl_remove_event_handler = function(handle)
    assert(handle == 91)
    removed += 1
    if mode ~= 'stuck_removal' then handler = nil end
end
env.nl_send_custom_event = function(name, value)
    assert(name == 'crml_persistent_f90c7812_own_event')
    sent += 1
    if handler then handler(owner, value) end
end
local chunk = function(...)
SOURCE
end
setfenv(chunk, env)
if mode == 'bad_environment' then env._ENV = {}; assert(chunk() == nil and added == 0); return end
local callback = chunk(mode == 'error')
assert(type(callback) == 'function' and added == 0 and removed == 0)
assert(callback() == 1 and callback() == 2 and removed == 0)
assert(added == 1)
local ok, result = pcall(callback)
if mode == 'error' then assert(not ok and type(result) == 'string')
else assert(ok and result == 3) end
-- The native host supplies a non-nil command to request explicit shutdown.
local stopped = callback(callback)
assert(removed == 1 and sent == 4)
if mode == 'stuck_removal' then assert(stopped == -401)
else assert(stopped == 3 and handler == nil) end
print('event lifecycle passed')
'''
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "persistent_events.luau"
            for mode in ("success", "error", "stuck_removal", "bad_environment"):
                with self.subTest(mode=mode):
                    script.write_text(host.replace('MODE', repr(mode)).replace('SOURCE', source), encoding="utf-8")
                    result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_persistent_closures(self):
        if VM is None:
            self.skipTest("supply --vm for standalone Lua behavior checks")
        for name in ("persistent", "persistent_error"):
            source = (ROOT / "examples/lua-probe" / (name + ".luau")).read_text(encoding="utf-8")
            host = '''
local environment = { self = {} }
environment._ENV = environment
setmetatable(environment, {__index = getfenv(0)})
local chunk = function()
%s
end
setfenv(chunk, environment)
local callback = chunk()
assert(type(callback) == 'function')
assert(callback() == 1 and callback() == 2)
local ok, value = pcall(callback)
if %s then assert(not ok and type(value) == 'string')
else assert(ok and value == 3 and callback() == 4) end
assert(rawget(getfenv(0), '__crml_persistent_counter') == nil)
assert(environment.__crml_persistent_counter == %d)
-- Reloading the source into another environment must start at one.
local replacement = {self = {}}
replacement._ENV = replacement
setmetatable(replacement, {__index = getfenv(0)})
setfenv(chunk, replacement)
local nextCallback = chunk()
assert(nextCallback() == 1)
assert(environment.__crml_persistent_counter == %d)
print('persistent passed')
''' % (source, 'true' if name.endswith('error') else 'false',
       3 if name.endswith('error') else 4, 3 if name.endswith('error') else 4)
            with self.subTest(script=name), tempfile.TemporaryDirectory() as tmp:
                script = Path(tmp) / "persistent.luau"
                script.write_text(host, encoding="utf-8")
                result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_event_cleanup_and_environment(self):
        if VM is None:
            self.skipTest("supply --vm for standalone Lua behavior checks")
        source = (ROOT / "examples/lua-probe/events.luau").read_text(encoding="utf-8")
        # This host deliberately retains dead list entries after removing their
        # function references, matching the reviewed deferred-removal behavior.
        host = '''
local mode = ...
local owner = {}
local update = function() end
local listener, target, name
local sends, removes = 0, 0
function nl_update_callback(...)
    assert(select('#', ...) == 0, "must never replace update")
    assert(getfenv(2).self == owner, "active script must have the owner")
    return update
end
function nl_add_event_handler(entity, event, callback)
    assert(entity == owner and getfenv(callback).self == owner)
    target, name, listener = entity, event, callback
    if mode == 'add_error' then listener = nil; error('add failed') end
    return 17
end
function nl_remove_event_handler(handle)
    assert(handle == 17)
    removes += 1
    if mode == 'remove_error' then error('remove failed') end
    listener = nil
end
function nl_send_custom_event(event, payload)
    sends += 1
    assert('lua.' .. event == name)
    if mode == 'dispatch_error' then error('dispatch failed') end
    if mode == 'second_error' and sends == 2 then error('second send failed') end
    if mode == 'no_delivery' then return end
    if listener then listener(target, mode == 'bad_payload' and 0 or payload) end
    if listener and mode == 'duplicate' then listener(target, payload) end
end
local probe = function(...)
%s
end
local missing = string.match(mode, '^missing_(.+)$')
-- Emulate the native loader supplying a rooted environment table, even when
-- all environment helper globals are absent from the guest's shared library.
local original = getfenv(0)
local disabled = { getfenv = true, setfenv = true, setmetatable = true,
    rawget = true, assert = true }
if missing then disabled[missing] = true end
-- The CLI environment inherits builtins. A shallow clone loses those entries;
-- resolve through the original environment while hiding only selected names.
local shared = setmetatable({}, { __index = function(_, key)
    if disabled[key] then return nil end
    return original[key]
end })
local env = setmetatable({ self = owner }, { __index = shared })
env._ENV = env
for key in disabled do assert(env[key] == nil, 'guest helper must be absent') end
assert(env.type == type, 'inherited builtins must remain available')
if mode == 'bad_environment' then env.self = nil end
setfenv(probe, env)
local ok, result, detail = pcall(probe, owner)
assert(rawget(shared, '__crml_probe_environment_v1') == nil)
assert((listener == nil) == (mode ~= 'remove_error'), "removal outcome must be truthful")
if mode == 'success' then
    assert(ok and result == 127 and sends == 2 and removes == 1)
    assert(rawget(env, '__crml_probe_environment_v1') == 42)
elseif mode == 'add_error' then
    assert(not ok and sends == 0 and removes == 0)
elseif missing then
    local codes = { pcall = -106, nl_update_callback = -107,
        nl_add_event_handler = -108, nl_send_custom_event = -109, nl_remove_event_handler = -110 }
    assert(ok and result == codes[missing] and sends == 0 and removes == 0)
elseif mode == 'bad_environment' then
    assert(ok and result == -210 and sends == 0 and removes == 0)
elseif mode == 'remove_error' then
    assert(ok and result == -202 and removes == 1 and sends == 1)
    assert(type(detail) == 'string' and string.find(detail, 'remove failed', 1, true))
else
    local codes = { dispatch_error = -201, no_delivery = -203,
        duplicate = -204, bad_payload = -205, second_error = -206 }
    assert(ok and result == codes[mode] and sends == (mode == 'second_error' and 2 or 1) and removes == 1)
    if mode == 'dispatch_error' or mode == 'second_error' then assert(type(detail) == 'string') end
end
print('passed', mode)
''' % source
        with tempfile.TemporaryDirectory() as tmp:
            modes = ["success", "dispatch_error", "bad_payload", "add_error", "no_delivery", "duplicate", "second_error", "remove_error", "bad_environment"]
            modes += ["missing_" + name for name in ("pcall", "nl_update_callback", "nl_add_event_handler", "nl_send_custom_event", "nl_remove_event_handler")]
            for mode in modes:
                # Luau CLI script arguments use -a; embed the finite test mode to
                # keep the host independent of CLI argument convention changes.
                script = Path(tmp) / "probe.luau"
                script.write_text(host.replace("local mode = ...", f"local mode = '{mode}'"), encoding="utf-8")
                with self.subTest(mode=mode):
                    result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("passed", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", type=Path, required=True)
    args = parser.parse_args()
    VM = args.vm.resolve()
    unittest.main(argv=[__file__])
