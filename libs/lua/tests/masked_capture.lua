local d,p=require('display'),require('raster_test')
local region=d.capture_region(0,0,240,240)
local function snapshot() return (p.display_snapshot(d.capture_region,region)) end
local function pattern(kind)
 d.clear('black')
 if kind==1 then d.clear('red')
 elseif kind==2 then
  for y=0,239,3 do d.fill_rect(y%70,y,100,1,'red') end
 elseif kind==3 then
  for x=0,239,2 do d.fill_rect(x,0,1,240,'red') end
 elseif kind==4 then
  d.fill_rect(0,0,1,240,'red');d.fill_rect(239,0,1,240,'blue')
 elseif kind==6 then
  for x=0,239,2 do d.fill_rect(x,0,1,240,'red') end
  d.fill_rect(239,0,1,240,'blue')
 elseif kind==5 then
  d.fill_polygon({{3,7},{201,31},{168,207},{37,222}},'red')
  d.fill_polygon({{30,40},{160,50},{145,184},{50,192}},'black')
 end
end
local function compare(opaque,masked)
 for _,key in ipairs({false,'black','red','blue'}) do
  for _,clip in ipairs({{0,240,0,240,0,0},{31,191,19,217,-7,11}}) do
   local top,bottom,left,right,x,y=table.unpack(clip)
   d.clear('green');d.draw_region(opaque,x,y,top,bottom,key or nil,left,right)
   local expected=snapshot()
   d.clear('green');d.draw_region(masked,x,y,top,bottom,key or nil,left,right)
   assert(snapshot()==expected,'masked capture pixels/runs')
  end
 end
end
for kind=0,6 do
 pattern(kind)
 local opaque=d.capture_region(0,0,240,240)
 local masked=d.capture_region(0,0,240,240,'black')
 compare(opaque,masked)
 pattern(kind)
 compare(d.capture_region(7,11,217,191),d.capture_region(7,11,217,191,'black'))
end
local function during_allocation(action,fn,native)
 collectgarbage('collect');collectgarbage('incremental')
 local pause=collectgarbage('param','pause',0)
 local mul=collectgarbage('param','stepmul',0)
 local step=collectgarbage('param','stepsize',0)
 collectgarbage('stop')
 local fired=false
 local garbage=setmetatable({}, {__gc=function()
  assert(p.in_call(native),'finalizer must run inside capture')
  fired=true;action()
 end})
 garbage=nil;collectgarbage('restart');fn();assert(fired)
 collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
 collectgarbage('param','stepsize',step)
end
local function reopen(width,height)
 d.deinit();p.display_fixture_size(width or 240,height or 240)
 package.loaded.display=nil;d=require('display')
end
for mode=1,10 do
 reopen();pattern(mode==2 and 3 or mode==8 and 6 or mode>=9 and 1 or 0)
 local native=d.capture_region
 local captured,expected
 during_allocation(function()
  if mode==1 then pattern(3)
  elseif mode==2 then pattern(0)
  elseif mode==3 then reopen();pattern(4)
  elseif mode==4 then reopen(120,120)
  elseif mode==5 then d.deinit()
  elseif mode==6 then
   pattern(5);local nested=d.capture_region(0,0,240,240,'black')
   d.clear('blue');d.draw_region(nested,0,0)
  elseif mode==9 then pattern(6) -- Same pixel capacity, more runs.
  elseif mode==10 then d.clear('blue') -- Same totals, changed content.
  else pattern(1) end
  if mode~=4 and mode~=5 then expected=d.capture_region(0,0,240,240) end
 end,function()
  local ok,value=pcall(native,0,0,240,240,'black')
  assert(ok==(mode~=4 and mode~=5),'capture allocation lifecycle')
  captured=value
 end,native)
 if expected then compare(expected,captured) end
end
-- Reacquire with a different stride while the requested rectangle still fits.
reopen();pattern(1)
local resized,expected
local native=d.capture_region
during_allocation(function()
 reopen(160,220)
 for y=0,219 do d.fill_rect(0,y,160,1,y%2==0 and 'red' or 'blue') end
 expected=d.capture_region(7,9,100,100)
end,function() resized=native(7,9,100,100,'black') end,native)
reopen();compare(expected,resized);resized=nil;expected=nil
reopen();pattern(5)
local before=snapshot()
p.oom(function() d.capture_region(0,0,240,240,'black') end)
assert(snapshot()==before,'failed capture changed framebuffer')
collectgarbage('collect')
local baseline=collectgarbage('count')
local held={}
for i=1,1000 do held[i]=false end
local exhausted=false
for i=1,1000 do
 local ok,value=pcall(d.capture_region,0,0,240,240,'black')
 if not ok then assert(value=='not enough memory');exhausted=true;break end
 held[i]=value
end
assert(exhausted,'regions must be VM charged')
held=nil;collectgarbage('collect')
assert(collectgarbage('count')<baseline+2,'capture quota recovery')
assert(d.capture_region(0,0,240,240,'black'))
-- Leave enough quota for one dense final region but not a bbox temporary
-- plus that region. Capture must succeed and preserve the full source image.
do
 pattern(1);collectgarbage('collect')
 local sample=d.capture_region(0,0,240,240,'black')
 local bytes=#p.mesh_snapshot(sample)
 sample=nil;collectgarbage('collect')
 local padding={};for i=1,256 do padding[i]=false end
 local target=2*1024*1024-bytes-32768
 collectgarbage('stop')
 local i=0
 while p.vm_used(d.capture_region)<target do
  i=i+1;padding[i]=p.reserve(8192)
 end
 print('capture tight quota used',p.vm_used(d.capture_region),bytes)
 local captured=d.capture_region(0,0,240,240,'black')
 padding=nil;collectgarbage('restart');collectgarbage('collect')
 d.clear('blue');d.draw_region(captured,0,0)
 local actual=snapshot();d.clear('red');assert(snapshot()==actual,'tight quota capture')
end
-- Host allocation/peak evidence; replacement includes GC, not device timing.
for _,kind in ipairs({0,1,3,5}) do
 pattern(kind)
 local function capture() d.capture_region(0,0,240,240,'black') end
 p.measure('masked_capture_'..kind..'_cold',capture,1,true)
 p.measure('masked_capture_'..kind..'_replace_gc',function()
  collectgarbage('collect');capture()
 end,1)
end
print('masked capture: pixels, runs, allocation finalizers and 2 MiB recovery passed')
