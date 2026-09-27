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
    listener = nil
end
function nl_send_custom_event(event, payload)
    sends += 1
    assert('lua.' .. event == name)
    if mode == 'dispatch_error' then error('dispatch failed') end
    if listener then listener(target, mode == 'bad_payload' and 0 or payload) end
end
local probe = function(...)
%s
end
local ok, result = pcall(probe, owner)
assert(rawget(getfenv(0), '__crml_probe_environment_v1') == nil)
assert(listener == nil, "listener must be released")
if mode == 'success' then
    assert(ok and result == 127 and sends == 2 and removes == 1)
elseif mode == 'add_error' then
    assert(not ok and sends == 0 and removes == 0)
else
    assert(not ok and sends == 1 and removes == 1)
end
print('passed', mode)
''' % source
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ("success", "dispatch_error", "bad_payload", "add_error"):
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
