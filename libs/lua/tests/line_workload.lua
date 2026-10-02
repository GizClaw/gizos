local d,p=require('display'),require('raster_test')
local function resize(w,h)
 d.deinit();p.display_fixture_size(w,h)
 package.loaded.display=nil;d=require('display')
end
local checked=0
for _,size in ipairs({{1,1},{2,3},{8,5},{8,8}}) do
 resize(size[1],size[2]);checked=checked+p.line_exhaustive(d.draw_line,'red')
end
resize(240,240)
local ranges=p.line_ranges(d.draw_line,'red')
local region=d.capture_region(0,0,240,240)
local function snapshot() return (p.display_snapshot(d.draw_line,region)) end
local function compare(x,y,X,Y)
 d.clear('black');p.line_reference(d.draw_line,x,y,X,Y)
 local expected=snapshot()
 d.clear('black');d.draw_line(x,y,X,Y,'red')
 assert(snapshot()==expected,'original Bresenham pixel phase')
end
local lines=p.material_workload(2)
local near,visible={},{}
for _,r in ipairs(lines) do
 compare(r[1],r[2],r[3],r[4]);compare(r[3],r[4],r[1],r[2])
 local inside=true
 for _,v in ipairs(r) do if v<0 or v>=240 then inside=false end end
 local target=inside and visible or near;target[#target+1]=r
end
for _,x in ipairs({-240,-1,0,1,119,239,240,480}) do
 for _,X in ipairs({-240,-1,0,1,119,239,240,480}) do
  for _,y in ipairs({-240,0,239,480}) do
   for _,Y in ipairs({-240,0,239,480}) do compare(x,y,X,Y) end
  end
 end
end
-- Public fractional inputs truncate toward zero before the integer walk.
compare(-100.9,-239.9,479.9,240.9);compare(239.9,0.9,-.9,239.9)
local function replay(records)
 for _,r in ipairs(records) do d.draw_line(r[1],r[2],r[3],r[4],'red') end
end
p.noalloc(function() replay(lines) end)
p.measure('line_workload_996_calls_6_poses',function() replay(lines) end,#lines)
p.measure('line_workload_near_140',function() replay(near) end,#near)
p.measure('line_workload_visible_856',function() replay(visible) end,#visible)
for _,r in ipairs({{-241,0,0,0},{481,0,0,0},{0,-241,0,0},{0,0,0,481}}) do
 assert(not pcall(d.draw_line,r[1],r[2],r[3],r[4],'red'))
end
-- Offscreen culling cannot bypass color getters or acquisition validation.
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
assert(not pcall(d.draw_line,-2,-2,-1,-1,closing))
print('line workload: '..checked..' exhaustive pixel/damage cases, '..ranges..' range cases plus real and boundary lines passed')
