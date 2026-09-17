local d=require('display')
local p=require('raster_test')
local function reopen()
 d.deinit();package.loaded.display=nil;d=require('display')
end
local shift={matrix={1,0,0,1,1,0}}
local identity={}
local source_shift={transform={x=1,y=0,scale=1,angle=0},grid=1}
local function bytes() collectgarbage('collect');return collectgarbage('count')*1024 end

-- Full public active size is accepted, while the >1024 path allocates no stage.
for _,count in ipairs({1025,65536}) do
 local m=p.mesh_capacity(count)
 p.noalloc(function() d.draw_mesh(m,shift);p.mesh_shifted(m,1) end)
 shift.matrix[5]=2
 p.noalloc(function() d.draw_mesh(m,shift);p.mesh_shifted(m,2) end)
 shift.matrix[5]=1
end
-- Source preparation is shared by the bounded stage and the original
-- two-pass fallback, with no allocation after warming either path.
for _,count in ipairs({0,1,1023,1024,1025,65536}) do
 local m=p.mesh_capacity(math.max(count,1))
 if count==0 then d.update_mesh(m,{},{}) end
 d.draw_mesh(m,source_shift)
 p.noalloc(function()
  source_shift.transform.x=source_shift.transform.x==1 and 2 or 1
  d.draw_mesh(m,source_shift);p.mesh_shifted(m,source_shift.transform.x)
 end)
end
source_shift.transform.x=1
collectgarbage('collect')

-- Cold growth OOM preserves an existing complete candidate and derived data.
local pts={{1,1},{6,1},{6,6},{1,6}}
local faces={{0,1,4,'red'}}
local m=d.compile_mesh(pts,faces,1024,1)
d.draw_mesh(m,{cache=true})
p.mesh_marker(m,true)
local saved=p.mesh_snapshot(m)
p.oom(function() d.draw_mesh(m,{cache=true,matrix=shift.matrix}) end)
assert(p.mesh_snapshot(m)==saved)
d.draw_mesh(m,{cache=true});assert(p.mesh_marker(m,false))
-- Every earlier staged vertex remains unpublished when the last one is invalid.
d.draw_mesh(m,shift)
d.update_mesh(m,{{1,1},{6,1},{6,6},{1000000,6}},faces)
saved=p.mesh_snapshot(m)
assert(not pcall(d.draw_mesh,m,{matrix={32,0,0,1,0,0},cache=true}))
assert(p.mesh_snapshot(m)==saved)
d.update_mesh(m,pts,faces)

-- New mesh objects share one bounded buffer. Identity needs none; release frees it.
reopen()
local full=p.mesh_capacity(1024)
d.draw_mesh(full,identity)
local before=bytes()
d.draw_mesh(full,shift)
local charged=bytes()-before
assert(charged>=16384 and charged<=16512,'stage accounting '..charged)
print('mesh stage 1024 charged VM bytes',charged)
local after=bytes();d.deinit();local released=after-bytes()
assert(released>=charged,'stage release '..released)
package.loaded.display=nil;d=require('display')
local small=p.mesh_capacity(1023)
d.draw_mesh(small,shift)
p.oom(function() d.draw_mesh(full,shift) end)
collectgarbage('collect');collectgarbage('stop')
local control_before=collectgarbage('count')*1024
d.draw_mesh(small,shift)
local control_bytes=collectgarbage('count')*1024-control_before
collectgarbage('restart');collectgarbage('collect');collectgarbage('stop')
local growth_before=collectgarbage('count')*1024
d.draw_mesh(full,shift)
local growth_peak=collectgarbage('count')*1024-growth_before
assert(growth_peak-control_bytes==charged,'growth peak '..growth_peak..' control '..control_bytes)
print('mesh stage 1023 to 1024 added peak VM bytes',growth_peak,'control',control_bytes)
collectgarbage('restart')
for _,count in ipairs({1,16,1024,1025}) do
 local other=p.mesh_capacity(count)
 p.noalloc(function() shift.matrix[5]=shift.matrix[5]==1 and 2 or 1
  d.draw_mesh(other,shift);p.mesh_shifted(other,shift.matrix[5]) end)
end
shift.matrix[5]=1

-- Force a finalizer in the cold draw's allocation, after options are supplied.
-- The finalizer may replace the same mesh's active data, draw another mesh,
-- or close/reopen Display. Its own writes are retained as ordinary side effects.
local function during_allocation(action,draw,native)
 local option_keys={'matrix','transform','x','y','scale','angle','grid',
  'left','right','top','bottom','offset_x','color','cache'}
 collectgarbage('collect');collectgarbage('incremental')
 local pause=collectgarbage('param','pause',0)
 -- Lua's zero-work incremental step completes the cycle, including finalizers.
 local mul=collectgarbage('param','stepmul',0)
 local step=collectgarbage('param','stepsize',0)
 collectgarbage('stop')
 local fired=false
 local garbage=setmetatable({}, {__gc=function()
  assert(p.in_call(native),'finalizer must enter from the actual draw binding')
  fired=true;action()
 end})
 garbage=nil
 collectgarbage('restart')
 draw()
 assert(fired,'finalizer must run during cold draw')
 assert(#option_keys==14)
 collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
 collectgarbage('param','stepsize',step)
end
for _,source in ipairs({false,true}) do for mode=1,6 do
 reopen()
 local changing=d.compile_mesh(pts,faces,mode==2 and 16 or 2048,1)
 local other=p.mesh_capacity(1024)
 local updated={{2,2},{7,2},{7,7}}
 local update_faces={{0,1,3,'red'}}
 if mode==5 then
  for i=4,1025 do updated[i]={i%7,i%5} end
 elseif mode==6 then updated={};update_faces={} end
 local function action()
  if mode==1 or mode==5 or mode==6 then d.update_mesh(changing,updated,update_faces)
  elseif mode==2 then d.draw_mesh(other,shift)
  elseif mode==3 then d.deinit()
  else reopen();d.update_mesh(changing,updated,update_faces) end
 end
 local transform=source and source_shift or shift
 local options={cache=true,matrix=transform.matrix,transform=transform.transform,grid=transform.grid}
 local native=d.draw_mesh
 during_allocation(action,function()
  local ok=pcall(native,changing,options)
  assert(ok==(mode~=3))
 end,native)
 if mode~=3 then
  local current=mode==2 and pts or updated
  local fs=mode==2 and faces or update_faces
  local ref=d.compile_mesh(current,fs)
  d.clear('black');d.draw_mesh(ref,transform);d.present({retained=true})
  d.clear('black');d.draw_mesh(changing,options)
  assert(d.present()==0,'reentrant current mesh pixels')
 end
end end
-- Trigger reentry in span-cache allocation after stage capacity is already warm.
-- A finalizer installs a complete candidate; the failing outer call retains it.
reopen()
local primed=p.mesh_capacity(1024);d.draw_mesh(primed,shift)
local late=d.compile_mesh({{1,1},{6,1},{6,6},{1000000,6}},{{0,1,3,'red'}},1024,1)
local invalid={cache=true,matrix={32,0,0,1,0,0}}
local native=d.draw_mesh
local installed
during_allocation(function()
 d.draw_mesh(late,{cache=true});p.mesh_marker(late,true);installed=p.mesh_snapshot(late)
end,function() assert(not pcall(native,late,invalid)) end,native)
assert(p.mesh_snapshot(late)==installed and p.mesh_marker(late,false))
-- A replayed static cache shrinks to its spans and keeps replaying the same
-- pixels; overflowing after that regrows it once to full capacity.
reopen()
local small=d.compile_mesh(pts,faces,4,1)
local cached={cache=true}
local function same(points)
 local ref=d.compile_mesh(points,faces)
 d.clear('black');d.draw_mesh(ref,identity);d.present({retained=true})
 d.clear('black');d.draw_mesh(small,cached)
 assert(d.present()==0,'cached mesh pixels')
end
d.draw_mesh(small,cached);d.draw_mesh(small,cached)
local full=bytes()
d.draw_mesh(small,cached)
assert(full-bytes()>=8000*20,'replayed span cache shrinks')
same(pts)
p.mesh_marker(small,true);d.draw_mesh(small,cached)
assert(p.mesh_marker(small,false),'shrunk cache replays')
local big={{1,1},{200,1},{200,200},{1,200}}
d.update_mesh(small,big,faces)
d.draw_mesh(small,cached)
local shrunk=bytes()
d.draw_mesh(small,cached);d.draw_mesh(small,cached);d.draw_mesh(small,cached)
assert(bytes()-shrunk>=8000*20,'overflowed span cache regrows and stays full')
same(big)
p.mesh_marker(small,true);d.draw_mesh(small,cached)
assert(p.mesh_marker(small,false),'regrown cache replays')
reopen()
local huge=p.mesh_capacity(65536)
p.noalloc(function() d.draw_mesh(huge,shift);p.mesh_shifted(huge,1) end)
d.deinit()
