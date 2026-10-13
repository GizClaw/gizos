-- Exercise rendering, timers and component callbacks without product code.
local runtime, display = require('runtime'), require('display')
local system, delay = require('system'), require('delay')
local downs, ups, frames, observed = 0, 0, 0, 0
runtime.components.on(tonumber(args.ok), runtime.event.BUTTON_DOWN, function(e)
    if e.timestamp_ms == e.pressed_at_ms then
        downs = downs + 1
        print('LUA_PROGRESS down=' .. downs)
    end
end)
runtime.components.on(tonumber(args.ok), runtime.event.BUTTON_UP, function()
    ups = ups + 1
    print('LUA_PROGRESS up=' .. ups)
end)
while true do
    local now = system.millis()
    if frames == 500 then print('LUA_PROGRESS ready') end
    if frames >= 500 and now - observed >= 100 then
        observed = now
        print('LUA_PROGRESS tick=' .. frames .. ' ups=' .. ups)
    end
    display.clear({r = 10, g = 13, b = 35})
    local phase = (frames < 500 or downs > ups) and frames or ups
    for i = 1, 24 do
        display.fill_rect((i * 7 + phase) % 220, (i * 11) % 220, 16, 16,
            {r = (phase + i) % 255, g = i * 7, b = 190})
    end
    display.present({retained = true, bounds = true, merge_gap = 0})
    frames = frames + 1
    delay.delay_ms(1)
end
