local d,p=require('display'),require('raster_test')
-- Six captured camera poses, ordinary projected wall-face quads only.
local flat=p.material_workload(true)
local quads={}
for i,r in ipairs(flat) do
 quads[i]={{r[1],r[2]},{r[3],r[4]},{r[5],r[6]},{r[7],r[8]}}
end
flat=nil
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
for i=1,#quads,113 do
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
p.measure('polygon_workload_732_calls_6_poses',replay,#quads)
print('polygon workload: '..#quads..' captured quads, boundary/legacy pixel oracle passed')
