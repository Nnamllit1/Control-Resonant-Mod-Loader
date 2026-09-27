-- Self-contained engine VM smoke test: no globals or engine calls.
local values = { 12, 9 }
return values[1] * 2 + values[2] * 2
