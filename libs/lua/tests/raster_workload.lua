-- Independent primitive pixel oracles and bounded generated inputs.
do
local d,p=require('display'),require('raster_test')
-- Generic convex quads spanning viewport boundaries and both windings.
local quads={}
for i=0,23 do
 local q={{-12+i,3+i%5},{230+i%7,10},{218-i%9,245-i},{7,230-i%3}}
 if i%2==1 then q[2],q[4]=q[4],q[2] end
 quads[#quads+1]=q
end
local region=d.capture_region(0,0,240,240)
local function snapshot() return (p.display_snapshot(d.fill_polygon,region)) end
local function compare(q,offset,top,bottom,scale)
 d.clear('black');p.polygon_reference(d.fill_polygon,q,offset,top,bottom,scale)
 local expected=snapshot()
 d.clear('black');d.fill_polygon(q,'red',offset,top,bottom,scale)
 assert(snapshot()==expected,'frozen polygon pixel oracle')
end
for i,q in ipairs(quads) do
 compare(q,0,0,240,1)
 compare(q,0,i%120,120+i%121,1)
end
local cases={
 {{0,0},{240,0},{240,240},{0,240}},
 {{0,0},{0,240},{240,240},{240,0}},
 {{1,1},{1,1},{200,200},{1,200}},
 {{1,1},{200,200},{200,1},{1,200}},
 {{10,10},{220,110},{10,220},{70,110}},
 {{10,10},{220,110},{100,110},{10,220}},
 {{10,10},{10,10},{10,10},{10,10}},
 {{-99999,-99999},{99999,-99998},{99998,99999},{-99999,99999}},
 {{1,10},{100,10+2^-30},{120,110},{1,110}},
 {{1,1},{110,1},{110,110}},
 {{1,1},{110,1},{110,110},{40,60},{1,110}},
}
for _,q in ipairs(cases) do
 for _,offset in ipairs({0,-.5,.5,0x1.ffffffffffffep-2,-1.125}) do
  for _,scale in ipairs({.125,1,16}) do compare(q,offset,0,240,scale) end
 end
end
for i=0,191 do
 local e=({0,2^-25,-2^-25,2^-24,-2^-24,.5})[i%6+1]
 local q={{-3+e,-7},{230+e,10+(i%3)*e},{210+(i%7)/8,241},{1+e,239}}
 if i%2==0 then q[2],q[4]=q[4],q[2] end
 compare(q,0,0,240,1);compare(q,0,i%120,120+i%121,1)
end
-- The same kernel also records mesh spans: cold packing, horizontal clips,
-- warm replay and shrinking cache must preserve the reference pixels.
for i=1,#quads do
 local q=quads[i]
 local mesh=d.compile_mesh(q,{{0,1,4,'red'}})
 local opts={left=17,right=219,top=11,bottom=228,cache=true}
 d.clear('black');p.polygon_reference(d.fill_polygon,q,0,11,228,1,17,219)
 local expected=snapshot()
 for _=1,4 do
  d.clear('black');d.draw_mesh(mesh,opts);assert(snapshot()==expected,'quad cached spans')
 end
 p.noalloc(function() d.draw_mesh(mesh,opts) end)
end
local function replay()
 for _,q in ipairs(quads) do d.fill_polygon(q,'red') end
end
p.noalloc(replay)
p.measure('polygon_generic_24_quads',replay,#quads)
print('polygon workload: '..#quads..' generic quads, boundary/legacy pixel oracle passed')
end
do
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
local lines={}
-- Exercise visible, crossing and wholly clipped segments in both directions.
for i=0,31 do
 lines[#lines+1]={-40+i*4,17+i*3,270-i*2,225-i*5}
 lines[#lines+1]={i*7%240,i*11%240,(i*13+27)%240,(i*17+9)%240}
end
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
p.measure('line_generic_64_segments',function() replay(lines) end,#lines)
p.measure('line_generic_clipped',function() replay(near) end,#near)
p.measure('line_generic_visible',function() replay(visible) end,#visible)
for _,r in ipairs({{-241,0,0,0},{481,0,0,0},{0,-241,0,0},{0,0,0,481}}) do
 assert(not pcall(d.draw_line,r[1],r[2],r[3],r[4],'red'))
end
-- Offscreen culling cannot bypass color getters or acquisition validation.
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
assert(not pcall(d.draw_line,-2,-2,-1,-1,closing))
print('line workload: '..checked..' exhaustive pixel/damage cases, '..ranges..' range cases plus generic and boundary lines passed')
end
