-- Deliberately fail on a later invocation, after two successful callbacks.
if self ~= _ENV.self or _ENV._ENV ~= _ENV then return nil end
__crml_persistent_counter = 0
return function()
    __crml_persistent_counter += 1
    if __crml_persistent_counter == 3 then
        local missing = nil
        missing()
    end
    return __crml_persistent_counter
end
