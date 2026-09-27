-- Presence checks only: none of these engine functions is invoked.
local mask = 0
if type(nl_update_callback) == "function" then mask += 1 end
if type(nl_add_event_handler) == "function" then mask += 2 end
if type(nl_send_custom_event) == "function" then mask += 4 end
if type(nl_copy_world_transform) == "function" then mask += 8 end
if type(spawn_bundle_with_instigator) == "function" then mask += 16 end
if type(npc_spawn) == "function" then mask += 32 end
if type(npc_despawn) == "function" then mask += 64 end
return mask
