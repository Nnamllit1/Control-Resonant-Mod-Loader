-- Runs once in the engine VM. The native harness passes the current update's
-- generation-checked entity as tagged light userdata; no entity is invented.
local owner = ...
-- Distinguish unavailable globals from an engine callback failure. Function
-- presence does not prove permission to call it or a compatible argument ABI.
if type(pcall) ~= "function" then return -106 end
if type(nl_update_callback) ~= "function" then return -107 end
if type(nl_add_event_handler) ~= "function" then return -108 end
if type(nl_send_custom_event) ~= "function" then return -109 end
if type(nl_remove_event_handler) ~= "function" then return -110 end
-- The native bridge supplies this environment, as the engine does for its own
-- scripts. Environment helper globals are not required or reintroduced.
-- A private environment is a namespace, not a security sandbox.
if self ~= owner or _ENV.self ~= owner or _ENV._ENV ~= _ENV then return -210 end
__crml_probe_environment_v1 = 42

-- No-argument form is a getter. Never replace the engine's update callback.
local originalUpdate = nl_update_callback()
if originalUpdate ~= nil and type(originalUpdate) ~= "function" then return -211 end
local name = "crml_probe_6e194312_own_event"
local count, payloadMatches = 0, true
local listener = nl_add_event_handler(owner, "lua." .. name, function(sender, value)
    count += 1
    payloadMatches = payloadMatches and sender == owner and value == 731
end)

-- Cleanup is attempted even if dispatch raises an error. Only our returned
-- handle is removed. Engine removal clears its Lua reference immediately;
-- removal of the event-list entry can be deferred.
local sent, sendError = pcall(function() nl_send_custom_event(name, 731) end)
local removed, removeError = pcall(function() nl_remove_event_handler(listener) end)
if not removed then return -202, removeError end
if not sent then return -201, sendError end
if count == 0 then return -203 end
if count ~= 1 then return -204 end
if not payloadMatches then return -205 end
local sentAgain, secondError = pcall(function() nl_send_custom_event(name, 731) end)
if not sentAgain then return -206, secondError end
if count ~= 1 then return -207 end
if nl_update_callback() ~= originalUpdate then return -208 end
if __crml_probe_environment_v1 ~= 42 then return -209 end
return 127
