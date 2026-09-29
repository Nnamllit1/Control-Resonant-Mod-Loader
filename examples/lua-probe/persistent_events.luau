-- Only this private event is sent. The listener counts deliveries and never
-- changes entities, initiates travel or replaces the game's update callback.
local failOnThird, failListener, failInitialization = ...
if failInitialization then failOnThird, failListener = false, false end
if self ~= _ENV.self or _ENV._ENV ~= _ENV then return nil end
if type(nl_add_event_handler) ~= "function" or type(nl_remove_event_handler) ~= "function"
    or type(nl_send_custom_event) ~= "function" or type(pcall) ~= "function" then return nil end
local owner = self
local name = "crml_persistent_f90c7812_own_event"
local calls, deliveries = 0, 0
local matches = true
local listener
local listenerFailed = false
local initialized, rollbackDone, stopped, cleanupFailed = false, false, false, false
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
local function removeListener()
    if cleanupFailed then return false end
    if listener then
        -- Never retry an ambiguous removal: engine handle numbers can recycle.
        local handle = listener
        listener = nil
        local ok = pcall(nl_remove_event_handler, handle)
        if not ok then cleanupFailed = true; return false end
    end
    return true
end
-- Construct the controller before creating engine-owned resources. The host
-- roots this closure first; allocation/retain failure has nothing to roll back.
return function(shutdown)
    if shutdown then
        stopped = true
        if not removeListener() then return -405 end
        local before = deliveries
        nl_send_custom_event(name, -1)
        if deliveries ~= before then return -401 end
        return calls
    end
    if stopped or cleanupFailed then return -406 end
    calls += 1
    if not initialized then
        local deliberate = false
        local ok = pcall(function()
            listener = nl_add_event_handler(owner, "lua." .. name, receive)
            if failInitialization and not rollbackDone then
                deliberate = listener ~= nil
                local missingInitialization = nil
                missingInitialization()
            end
        end)
        if not ok then
            if not removeListener() then return -405 end
            local before = deliveries
            nl_send_custom_event(name, -1)
            if deliveries ~= before then return -401 end
            if not deliberate then return -404 end
            rollbackDone = true
            return -410 -- Deliberate failure cleaned up; retry on a later update.
        end
        if not listener then return -404 end
        initialized = true
    end
    nl_send_custom_event(name, calls)
    local expectedDeliveries = if failListener and calls >= 3 then 3 else calls - (if rollbackDone then 1 else 0)
    if not matches or deliveries ~= expectedDeliveries then return -402 end
    if failListener and calls >= 3 and not listenerFailed then return -403 end
    if failOnThird and not failListener and calls == 3 then
        local missing = nil
        missing()
    end
    return calls
end
