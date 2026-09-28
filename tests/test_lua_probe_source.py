"""Check the self-authored event script against a standalone Luau mock host."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
VM = None


class SourceTests(unittest.TestCase):
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
local shared = table.clone(getfenv(0))
shared.getfenv, shared.setfenv, shared.setmetatable, shared.rawget, shared.assert = nil, nil, nil, nil, nil
if missing then shared[missing] = nil end
local env = setmetatable({ self = owner }, { __index = shared })
env._ENV = env
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
                result = subprocess.run([str(VM), str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("passed", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", type=Path, required=True)
    args = parser.parse_args()
    VM = args.vm.resolve()
    unittest.main(argv=[__file__])
