-- A trusted engine-Lua controller. Its private environment and local state
-- survive between updates. This example has no visible gameplay effect.
local updates = 0
local stopped = false

-- Construction creates no engine resources. The runtime retains this closure
-- before its first update, so acquisition belongs inside the callback.
return function(shutdown)
    if shutdown then
        stopped = true
        _ENV.example_update_count = nil
        return
    end
    if stopped then return end

    updates += 1
    _ENV.example_update_count = updates
end
