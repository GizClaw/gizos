-- Identical source runs on a native worker Host and a Web inline Host.
local d, runtime = require('display'), require('runtime')
local function submit(options)
  local sequence, code = d.submit(options)
  while not sequence do
    assert(code == d.BUSY, 'display submit fault: '..tostring(code))
    runtime.sleep(1)
    sequence, code = d.submit(options)
  end
  return sequence
end
local function flush()
  local record, code = d.flush()
  while not record do
    assert(code == d.BUSY, 'display flush fault: '..tostring(code))
    runtime.sleep(1)
    record, code = d.flush()
  end
  assert(record.error == 0 and not record.busy)
  return record
end

d.clear('black')
assert(submit() == 1)
flush()
d.fill_rect(0, 0, 32, 32, 'red')
assert(submit() == 2)
-- Drawing is permitted before transport completes; it cannot change frame 2.
d.fill_rect(0, 0, 32, 32, 'blue')
assert(submit() == 3)
local record = flush()
assert(record.submitted == 3 and record.completed == 3)
assert(record.successful == 3 and record.changed_frames == 3)
assert(submit() == 4)
record = flush()
assert(record.completed == 4 and record.changed_frames == 3)
assert(record.pixels == 0 and record.rects == 0)
-- Existing synchronous API still finishes the current frame before returning.
d.fill_rect(32, 0, 32, 32, 'red')
local pixels, rects = d.present({retained=true})
assert(pixels > 0 and rects > 0)
record = d.status()
assert(record.completed == 5 and record.changed_frames == 4)
assert(record.clock_valid and record.completed_us >= record.started_us)
print('LUA_SUBMIT contract=PASS')
if args.hold == 'yes' then
  while true do runtime.sleep(100) end
end
