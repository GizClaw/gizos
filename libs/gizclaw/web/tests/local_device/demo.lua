local runtime = require('runtime')
local display = require('display')
local kv = require('kv')
local capability = require('capability')
local visits = kv.get('visits') or 0
assert(kv.set('visits', visits + 1))
display.clear({r = 10, g = 60, b = 130})
display.draw_text(12, 24, 'LEASE ' .. tostring(visits + 1), {font_size = 18})
display.present()
runtime.sleep(2)
local ok, value, err = capability.call('fixture.finish', tostring(visits + 1), nil)
assert(ok, err)
return 'natural'
