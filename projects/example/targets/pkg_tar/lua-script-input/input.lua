-- Counts OK Button presses (first Down sample) and Up edges so the browser test can prove that page
-- input sources merge into one pressed state and are released on blur and at
-- teardown.
local runtime = require("runtime")
local display = require("display")

local downs, ups = 0, 0
-- Runtime publishes a Down sample on every poll while held; only the first
-- sample of a press has timestamp_ms == pressed_at_ms.
runtime.components.on(tonumber(args.ok), runtime.event.BUTTON_DOWN, function(event)
    if event.timestamp_ms ~= event.pressed_at_ms then return end
    downs = downs + 1
    print("H2_WEB_LUA_APP_INPUT ok=down " .. downs)
end)
runtime.components.on(tonumber(args.ok), runtime.event.BUTTON_UP, function()
    ups = ups + 1
    print("H2_WEB_LUA_APP_INPUT ok=up " .. ups)
end)
while true do
    display.clear({r = 10, g = 13, b = 35})
    display.fill_rect(40, 100, 160, 40, downs > ups and {r = 29, g = 78, b = 216} or {r = 40, g = 44, b = 65})
    display.draw_text(52, 112, "OK " .. downs .. "/" .. ups, {font_size = 14})
    display.present()
    runtime.sleep(50)
end
