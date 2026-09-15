local d=require('display')
local p=require('raster_test')
local g=require('geometry')
local v=require('vmath')
local function bad(...) assert(not pcall(...)) end
local vertices={{-2,-1},{3,-1},{2,4},{-1,3}}
local faces={{0,1,4,'red'},{1,1,2,'blue'}}
local m=d.compile_mesh(vertices,faces,8,3)
local opts={transform={x=5,y=5,scale=1,angle=0},grid=1,cache=true}
local function compare(points,t,grid)
 d.update_mesh(m,points,faces)
 local expected=p.mesh_source(points,t.x,t.y,t.scale,t.angle,grid)
 local ref=d.compile_mesh(expected,faces)
 d.clear('black');d.draw_mesh(ref);d.present({retained=true})
 opts.transform=t;opts.grid=grid
 for _=1,2 do
  d.clear('black');d.draw_mesh(m,opts);assert(d.present()==0,'source transform pixels')
  p.mesh_source(points,t.x,t.y,t.scale,t.angle,grid,m)
 end
end
-- Independent float/error/fallback oracle around signed half-grid boundaries.
for grid=1,16 do
 for _,angle in ipairs({0,.13,-.73,3.141592653589793,100000,-100000}) do
  for _,delta in ipairs({-1e-5,-1e-12,0,1e-12,1e-5}) do
   compare(vertices,{x=10+(grid/2+delta),y=10-grid/2-delta,scale=.8,angle=angle},grid)
  end
 end
end
local unit={{0,0},{1,0},{1,1},{0,1}}
for grid=1,16 do
 for _,sign in ipairs({-1,1}) do
  for _,delta in ipairs({-1e-5,-1e-12,0,1e-12,1e-5}) do
   compare(unit,{x=sign*grid/2+delta,y=-sign*grid/2-delta,scale=1,angle=0},grid)
  end
 end
end
-- Large cancelling inputs straddle the .25 error gate while remaining visible.
for _,magnitude in ipairs({1000,8190,8192,8194,10922.2,10922.333333333,10922.4,16384,99999,100000,100001,1000000}) do
 for _,scale in ipairs({.00001,.1,1}) do
  local pts={{magnitude,magnitude},{magnitude+1,magnitude},{magnitude,magnitude+1},{magnitude-1,magnitude+2}}
  if magnitude==1000000 then pts={{magnitude,0},{magnitude-1,0},{magnitude-1,1},{magnitude,1}} end
  compare(pts,{x=-math.min(magnitude*scale,100000),y=0,scale=scale,angle=0},1)
 end
end
for i=1,100 do
 compare(vertices,{x=20+(i%7)*.11,y=22-(i%11)*.09,scale=.03+(i%23)*.7,angle=i*.17},1+i%16)
end
-- Cache tests observe replay records, not dirty-pixel equality.
local verts={{1,1},{6,1},{6,6},{1,6}}
d.update_mesh(m,verts,faces)
local stable={cache=true,grid=1,transform={x=0,y=0,scale=1,angle=0}}
d.draw_mesh(m,stable)
local function hit(change,options)
 p.mesh_marker(m,true);change();d.draw_mesh(m,options or stable)
 assert(p.mesh_marker(m,false),'expected span replay')
end
local function miss(change,options)
 p.mesh_marker(m,true);change();d.draw_mesh(m,options or stable)
 assert(not p.mesh_marker(m,false),'expected reraster')
end
hit(function() end)
hit(function() stable.transform.x=.01 end)
hit(function() d.update_mesh(m,{{1.01,1},{6,1},{6,6},{1,6}},faces) end)
hit(function() d.update_mesh(m,{{2,2},{7,2},{7,7},{2,7}},faces);d.update_mesh(m,verts,faces) end)
hit(function() bad(d.update_mesh,m,{{0,0},{0/0,2}},faces) end)
hit(function()
 d.update_mesh(m,{{1,1},{6,1},{6,6},{1000000,6}},faces)
 bad(d.draw_mesh,m,{matrix={32,0,0,1,0,0}})
 d.update_mesh(m,verts,faces)
end)
miss(function() stable.transform.x=2 end)
miss(function() stable.transform.x=0 end)
miss(function() faces[1][4]='green';d.update_mesh(m,verts,faces) end)
miss(function() faces[1][3]=3;d.update_mesh(m,verts,faces) end)
miss(function() faces[2][2]=2;d.update_mesh(m,verts,faces) end)
miss(function() table.remove(faces);d.update_mesh(m,verts,faces) end)
miss(function() verts[5]={3,3};d.update_mesh(m,verts,faces) end)
-- Identity/nonidentity paths compare final coordinates, including after updates.
hit(function() end,{cache=true})
hit(function() end,stable)
hit(function() d.draw_mesh(m,{matrix={1,0,0,1,50,0}}) end)
for _,field in ipairs({'left','right','top','bottom','offset_x','color'}) do
 local values={left=2,right=5,top=2,bottom=5,offset_x=.5,color='blue'}
 miss(function() stable[field]=values[field] end)
 miss(function() stable[field]=nil end)
end
hit(function() d.draw_mesh(m,{left=0,right=0,cache=true}) end)
p.noalloc(function() stable.transform.x=stable.transform.x==0 and .01 or 0;d.draw_mesh(m,stable) end)
local identity={}
p.noalloc(function() d.draw_mesh(m,identity) end)

-- Public numeric-buffer updates exercise the actual per-frame native producer.
local writer,handle=g.mesh(4,1)
local xy=v.buffer(8);xy:load{1,1,6,1,6,6,1,6}
local topology=v.buffer(4);topology:load{0,1,4,63488}
g.update_mesh(writer,xy,topology,4,1);d.draw_mesh(handle,stable)
p.mesh_marker(handle,true)
p.noalloc(function() g.update_mesh(writer,xy,topology,4,1);d.draw_mesh(handle,stable) end)
assert(p.mesh_marker(handle,false),'native update lost equivalent spans')
xy:set(1,2);g.update_mesh(writer,xy,topology,4,1);d.draw_mesh(handle,stable)
assert(not p.mesh_marker(handle,false),'native coordinate change reused old spans')

-- Exact positive/negative public result limits and late out-of-range failure.
local bounds={{0,0},{160000,0}}
local edge=d.compile_mesh(bounds,{{1,1,2,'red'}})
local limit={transform={x=0,y=0,scale=100,angle=0},grid=1,cache=true}
d.draw_mesh(edge,limit);p.mesh_source(bounds,0,0,100,0,1,edge)
bounds[2][1]=-160000;d.update_mesh(edge,bounds,{{1,1,2,'red'}})
d.draw_mesh(edge,limit);p.mesh_source(bounds,0,0,100,0,1,edge)
bounds[2][1]=160001;d.update_mesh(edge,bounds,{{1,1,2,'red'}})
bad(d.draw_mesh,edge,limit)

-- Full validation applies to hidden/empty clips, preserves pixels and candidates.
d.clear('black');d.draw_mesh(m,stable);d.present()
for _,t in ipairs({false,{}, {x=0,y=0,scale=0,angle=0},
 {x=0,y=0,scale=100.01,angle=0}, {x=0/0,y=0,scale=1,angle=0},
 {x=0,y=100001,scale=1,angle=0}, {x=0,y=0,scale=1,angle=math.huge}}) do
 bad(d.draw_mesh,m,{transform=t,grid=1,cache=true});assert(d.present()==0)
end
for _,grid in ipairs({0,-1,17,1.5}) do bad(d.draw_mesh,m,{transform=stable.transform,grid=grid}) end
bad(d.draw_mesh,m,{transform=stable.transform})
bad(d.draw_mesh,m,{transform=stable.transform,grid=1,matrix={1,0,0,1,0,0}})
local raw=setmetatable({}, {__index=function() error('unexpected transform getter') end})
bad(d.draw_mesh,m,{transform=raw,grid=1})
d.update_mesh(m,{{1,1},{6,1},{6,6},{1000000,6}},faces)
bad(d.draw_mesh,m,{transform={x=0,y=0,scale=100,angle=0},grid=1,left=0,right=0})
assert(d.present()==0)
d.update_mesh(m,verts,faces)
-- Cache allocation failure publishes no pixels; repeated cache-free drawing works.
local fresh=d.compile_mesh({{1,1},{6,1},{6,6}},{{0,1,3,'red'}})
local cold={transform={x=0,y=0,scale=1,angle=0},grid=1,cache=true}
p.oom(function() d.draw_mesh(fresh,cold) end);assert(d.present()==0)
d.draw_mesh(fresh,cold)
local weak=setmetatable({fresh},{__mode='v'});fresh=nil;collectgarbage('collect');assert(not weak[1])
-- Empty updates and restoration revalidate counts, even with retained candidates.
d.update_mesh(m,{},{});d.clear('black');d.present();d.draw_mesh(m,stable);assert(d.present()==0)
d.update_mesh(m,verts,faces);d.draw_mesh(m,stable)
-- A getter may update geometry and collect while options are decoded.
local color=setmetatable({}, {__index=function()
 d.update_mesh(m,{{2,2},{7,2},{7,7},{2,7}},faces);collectgarbage('collect');return 255
end})
d.draw_mesh(m,{color=color,cache=true,grid=1,transform={x=0,y=0,scale=1,angle=0}})
local ref=d.compile_mesh({{2,2},{7,2},{7,7},{2,7}},faces)
d.clear('black');d.draw_mesh(ref,{color='white'});d.present()
d.clear('black');d.draw_mesh(m,{color='white',cache=true});assert(d.present()==0)
-- Shutdown through a color getter must still block publication.
local closing=setmetatable({}, {__index=function() d.deinit();collectgarbage('collect');return 0 end})
bad(d.draw_mesh,m,{color=closing,cache=true,grid=1,transform={x=0,y=0,scale=1,angle=0}})
bad(d.draw_mesh,m,stable)
