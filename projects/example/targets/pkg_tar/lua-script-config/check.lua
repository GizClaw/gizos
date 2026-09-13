-- Generic shared-entry configuration regression; no game-specific dependency.
local runtime = require("runtime")
local display = require("display")
assert(args.profile == "procedural" and args.empty == "")
assert(args.escaped == 'quote=" slash=\\')
assert(args.trigraph == "??/")
assert(tonumber(args.left) == 1 and tonumber(args.ok) == 2)
assert(tonumber(args.back) == 3)
local retained = string.rep("v", 768 * 1024)
print("LUA_CONFIG args=PASS allocation=" .. #retained)
while true do
    assert(#retained == 786432 and retained:sub(-1) == "v")
    display.clear({r = 10, g = 30, b = 60})
    display.fill_rect(20, 20, 200, 200, {r = 30, g = 160, b = 240})
    display.present()
    runtime.sleep(50)
end
-- SOURCE_PADDING
