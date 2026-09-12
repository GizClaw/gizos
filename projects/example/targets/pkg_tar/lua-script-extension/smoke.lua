-- Smoke script for h2_lua_web_app(): checks its Button args and the extension
-- capability, then animates until the exit Button ends the job.
local runtime = require("runtime")
local display = require("display")
local capability = require("capability")

assert(tonumber(args.left) == 1 and tonumber(args.ok) == 2)
assert(tonumber(args.back) == 3)
local ok, output = capability.call("smoke.echo", "ping")
assert(ok and output == "ping", "extension capability unavailable")

local x, step = 0, 4
runtime.components.on(tonumber(args.ok), runtime.event.BUTTON_UP, function()
    step = -step
end)
while true do
    x = (x + step) % 200
    display.clear({r = 10, g = 13, b = 35})
    display.fill_rect(20 + x, 110, 20, 20, {r = 80, g = 200, b = 255})
    display.draw_text(20, 20, "LUA SCRIPT", {font_size = 14})
    display.present()
    runtime.sleep(50)
end
