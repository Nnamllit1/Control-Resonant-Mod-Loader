-- Only this private event is sent. The listener counts deliveries and never
-- changes entities, initiates travel or replaces the game's update callback.
local failOnThird = ...
if self ~= _ENV.self or _ENV._ENV ~= _ENV then return nil end
if type(nl_add_event_handler) ~= "function" or type(nl_remove_event_handler) ~= "function"
    or type(nl_send_custom_event) ~= "function" then return nil end
local owner = self
local name = "crml_persistent_f90c7812_own_event"
local calls, deliveries = 0, 0
local matches = true
local function receive(sender, value)
    deliveries += 1
    matches = matches and sender == owner and value == calls
end
local listener = nl_add_event_handler(owner, "lua." .. name, receive)
return function(shutdown)
    if shutdown then
        if listener then
            nl_remove_event_handler(listener)
            listener = nil
        end
        local before = deliveries
        nl_send_custom_event(name, -1)
        if deliveries ~= before then return -401 end
        return calls
    end
    calls += 1
    nl_send_custom_event(name, calls)
    if not matches or deliveries ~= calls then return -402 end
    if failOnThird and calls == 3 then
        local missing = nil
        missing()
    end
    return calls
end
