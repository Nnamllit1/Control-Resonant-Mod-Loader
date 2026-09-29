-- Only this private event is sent. The listener counts deliveries and never
-- changes entities, initiates travel or replaces the game's update callback.
local failOnThird, failListener = ...
if self ~= _ENV.self or _ENV._ENV ~= _ENV then return nil end
if type(nl_add_event_handler) ~= "function" or type(nl_remove_event_handler) ~= "function"
    or type(nl_send_custom_event) ~= "function" then return nil end
local owner = self
local name = "crml_persistent_f90c7812_own_event"
local calls, deliveries = 0, 0
local matches = true
local listener
local listenerFailed = false
local function receive(sender, value)
    deliveries += 1
    matches = matches and sender == owner and value == calls
    if failListener and calls == 3 then
        -- The engine owns error removal. Drop our handle before throwing so
        -- shutdown cannot call remove on a record the dispatcher has retired.
        listener = nil
        listenerFailed = true
        local missingListener = nil
        missingListener()
    end
end
listener = nl_add_event_handler(owner, "lua." .. name, receive)
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
    local expectedDeliveries = if failListener and calls >= 3 then 3 else calls
    if not matches or deliveries ~= expectedDeliveries then return -402 end
    if failListener and calls >= 3 and not listenerFailed then return -403 end
    if failOnThird and not failListener and calls == 3 then
        local missing = nil
        missing()
    end
    return calls
end
