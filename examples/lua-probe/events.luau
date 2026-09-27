-- Runs once in the engine VM. The native harness passes the current update's
-- generation-checked entity as tagged light userdata; no entity is invented.
local owner = ...
local shared = getfenv(1)
local env = setmetatable({ self = owner }, { __index = shared })
env._ENV = env
setfenv(1, env)

-- A private environment is a namespace, not a security sandbox.
local key = "__crml_probe_environment_v1"
local previous = rawget(shared, key)
env[key] = 42
assert(getfenv(1) == env and env[key] == 42 and rawget(shared, key) == previous)

-- No-argument form is a getter. Never replace the engine's update callback.
local originalUpdate = nl_update_callback()
assert(originalUpdate == nil or type(originalUpdate) == "function")
local name = "crml_probe_6e194312_own_event"
local count, payloadMatches = 0, true
local listener = nl_add_event_handler(owner, "lua." .. name, function(sender, value)
    count += 1
    payloadMatches = payloadMatches and sender == owner and value == 731
end)

-- Cleanup is attempted even if dispatch raises an error. Only our returned
-- handle is removed. Engine removal clears its Lua reference immediately;
-- removal of the event-list entry can be deferred.
local sent = pcall(function() nl_send_custom_event(name, 731) end)
local removed = pcall(function() nl_remove_event_handler(listener) end)
assert(removed, "CRML probe listener removal failed")
assert(sent and count == 1 and payloadMatches, "CRML probe delivery mismatch")
nl_send_custom_event(name, 731)
assert(count == 1, "CRML probe removed listener was invoked")
assert(nl_update_callback() == originalUpdate, "CRML probe update callback changed")
assert(rawget(shared, key) == previous, "CRML probe shared environment changed")
return 127
