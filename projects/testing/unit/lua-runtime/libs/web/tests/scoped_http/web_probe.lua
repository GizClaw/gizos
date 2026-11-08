local kv = require("kv")
local storage = require("storage")
local visits = (kv.get("visits") or 0) + 1
assert(kv.set("visits", visits))
assert(storage.write_file("visits", tostring(visits)))
assert(storage.read_file("visits") == tostring(visits))
return visits
