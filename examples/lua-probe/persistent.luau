-- Private environment retained by the returned closure. The native diagnostic
-- invokes it on later eligible updates; it never replaces nl_update_callback.
if self ~= _ENV.self or _ENV._ENV ~= _ENV then return nil end
__crml_persistent_counter = 0
return function()
    __crml_persistent_counter += 1
    return __crml_persistent_counter
end
