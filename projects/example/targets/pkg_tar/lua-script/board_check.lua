-- Board check for the web-board-* pages: reports the display size the board
-- gave the page, reports each board Button it receives, and fills the screen.
local runtime = require("runtime")
local display = require("display")

print("H2_WEB_BOARD_CHECK size=" .. display.width .. "x" .. display.height)
for name, id in pairs(args) do
    runtime.components.on(tonumber(id), runtime.event.BUTTON_UP, function()
        print("H2_WEB_BOARD_CHECK button=" .. name)
    end)
end
local frame = 0
while true do
    frame = frame + 1
    display.clear({r = 10, g = 13, b = 35})
    display.fill_rect(8, 8, display.width - 16, display.height - 16, {r = 30, g = 64, b = 120})
    display.fill_rect(16 + (frame * 4) % (display.width - 48), display.height // 2 - 8, 16, 16, {r = 250, g = 200, b = 80})
    display.present()
    runtime.sleep(50)
end
