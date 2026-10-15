local d,p=require('display'),require('raster_test')
local cached={cache=true}
local function rectangle(height)
 return {{1,1},{20,1},{20,1+height},{1,1+height}}
end
local face={{0,1,4,'red'}}
local region=d.capture_region(0,0,240,240)
local function snapshot() return (p.display_snapshot(d.draw_mesh,region)) end
local function pixels(mesh,options)
 local opts=options or cached
 opts.cache=false;d.clear('black');d.draw_mesh(mesh,opts);local expected=snapshot()
 opts.cache=true;d.clear('black');d.draw_mesh(mesh,opts)
 assert(snapshot()==expected,'mesh cache pixels')
end
-- Ordinary small geometry, with no font/game-specific data or classification.
local meshes={}
for i=1,50 do meshes[i]=d.compile_mesh(rectangle(6+i%13),face) end
local function cold_group() for _,m in ipairs(meshes) do d.draw_mesh(m,cached) end end
p.measure('mesh_cache_50_small_cold',cold_group,50,true)
local payload,capacity=0,0
for _,m in ipairs(meshes) do
 local cap,count,valid,overflow,_,_,_,bytes=p.mesh_cache_stats(m)
 assert(valid==1 and overflow==0 and count<=cap)
 payload=payload+bytes;capacity=capacity+cap
 pixels(m);p.noalloc(function() d.draw_mesh(m,cached) end)
end
print('mesh cache 50 cold payload bytes',payload,'slots',capacity)
meshes=nil;collectgarbage('collect')
-- Large cold geometry keeps the original 512 -> 2048 -> 8192 progression.
local many={};for i=1,150 do many[i]={0,1,4,i%2==0 and 'red' or 'blue'} end
local large=d.compile_mesh(rectangle(20),many)
for i,expected in ipairs({512,2048,8192}) do
 p.measure('mesh_cache_large_cold_'..i,function() d.draw_mesh(large,cached) end,1,true)
 local cap,_,valid,overflow=p.mesh_cache_stats(large)
 assert(cap==expected and valid==(i==3 and 1 or 0) and overflow==(i==3 and 0 or 1))
end
pixels(large)
d.draw_mesh(large,cached)
local cap,count,valid,overflow,_,shrunk=p.mesh_cache_stats(large)
assert(cap==3000 and count==3000 and valid==1 and overflow==0 and shrunk==1)
d.update_mesh(large,rectangle(22),many);pixels(large);pixels(large)
local full_cap,_,_,_,_,_,full=p.mesh_cache_stats(large)
assert(full_cap==8192 and full==1)
p.noalloc(function() d.draw_mesh(large,cached) end)
-- More than 8192 records still draws all primitives, never a partial replay.
d.update_mesh(large,rectangle(100),many)
for _=1,4 do pixels(large) end
local _,_,valid,overflow=p.mesh_cache_stats(large)
assert(valid==0 and overflow==1)
p.noalloc(function() d.draw_mesh(large,cached) end)
large=nil;collectgarbage('collect')
-- Fresh large meshes for all 32 timed samples; compilation stays outside
-- timing. All retain the baseline 512-slot first overflow, with no new stage.
local cold_pool={}
for i=1,36 do cold_pool[i]=d.compile_mesh(rectangle(20),many) end
p.measure('mesh_cache_large_fresh_32',function(i) d.draw_mesh(cold_pool[i+1],cached) end,1)
for _,m in ipairs(cold_pool) do
 local cap,_,valid,overflow=p.mesh_cache_stats(m)
 assert(cap==512 and valid==0 and overflow==1)
end
cold_pool=nil;collectgarbage('collect')
-- Transformed first draws retain the original initial-size/cold-cost policy.
local transformed=d.compile_mesh(rectangle(10),face)
d.draw_mesh(transformed,{cache=true,grid=1})
assert(p.mesh_cache_stats(transformed)==512)
-- No implicit initial allocation on uncached draws, and no loss of candidates
-- if an initial/growing allocation fails.
local fresh=d.compile_mesh(rectangle(10),face)
d.clear('black');local before=snapshot()
p.oom(function() d.draw_mesh(fresh,cached) end)
assert(snapshot()==before)
pixels(fresh)
d.update_mesh(fresh,rectangle(200),face);pixels(fresh)
local _,_,_,overflow=p.mesh_cache_stats(fresh)
if overflow==1 then
 local saved=p.mesh_snapshot(fresh)
 p.oom(function() d.draw_mesh(fresh,cached) end)
 assert(p.mesh_snapshot(fresh)==saved)
end
-- Explicit conservative bounds, including self-crossing even-odd polygons,
-- single cached lines, empty geometry, clipped rows and bounded inspection.
local shapes={
 {rectangle(10),face,20},
 {{{1,1},{20,1},{1,11}},{{0,1,3,'red'}},16},
 {{{1,1},{20,31},{20,1},{1,31}},face,60},
 {{{1,1},{20,200}},{{1,1,2,'red'}},16},
 {{},{},16},
 {rectangle(230),face,16,{cache=true,top=4,bottom=7}},
}
for _,case in ipairs(shapes) do
 local m=d.compile_mesh(case[1],case[2]);pixels(m,case[4])
 local cap,_,valid,overflow=p.mesh_cache_stats(m)
 assert(cap==case[3] and valid==1 and overflow==0,'initial conservative bound')
end
local hidden={};for i=1,33 do hidden[i]={0,1,4,'red'} end
local complex=d.compile_mesh({{1,300},{20,300},{20,320},{1,320}},hidden)
d.draw_mesh(complex,cached);assert(p.mesh_cache_stats(complex)==512,'bounded inspection')
local function during_allocation(action,fn,native)
 -- Keep option keys rooted to avoid extra string allocations; the handler
 -- below distinguishes their GC safepoints from cache allocation.
 local keys={'matrix','transform','x','y','scale','angle','grid',
  'left','right','top','bottom','offset_x','color','cache'}
 collectgarbage('collect');collectgarbage('incremental')
 local pause=collectgarbage('param','pause',0)
 local mul=collectgarbage('param','stepmul',0)
 local step=collectgarbage('param','stepsize',0)
 collectgarbage('stop')
 local fired,attempts=false,0
 local arm
 arm=function()
  local garbage=setmetatable({}, {__gc=function()
   assert(p.in_call(native));attempts=attempts+1;assert(attempts<32)
   if p.mesh_cache_allocating(native) then fired=true;action()
   else arm() end
  end})
  garbage=nil
 end
 -- lua_pushstring may run GC even for an interned option key. Re-arm across
 -- those safepoints until the native stack contains the allocated candidate.
 arm();collectgarbage('restart');fn();assert(fired and #keys==14)
 collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
 collectgarbage('param','stepsize',step)
end
for mode=1,3 do
 local m=d.compile_mesh(rectangle(4),face)
 local native=d.draw_mesh
 during_allocation(function()
  if mode==3 then d.deinit()
  else
   d.update_mesh(m,rectangle(200),face)
   if mode==2 then native(m,cached) end
  end
 end,function() assert(pcall(native,m,cached)==(mode~=3)) end,native)
 if mode==1 then
  local cap,_,valid,overflow=p.mesh_cache_stats(m)
  assert(cap==16 and valid==0 and overflow==1,'post-estimate change uses overflow: '..cap..','..valid..','..overflow)
  for _=1,4 do pixels(m) end
 elseif mode==2 then
  local cap,_,valid,overflow=p.mesh_cache_stats(m)
  assert(cap==400 and valid==1 and overflow==0,'reentrant candidate retained')
  pixels(m)
 else package.loaded.display=nil;d=require('display') end
end
-- Fifty one-shot small caches fit in 96 KiB of remaining real VM quota.
-- This deliberately cannot hold fifty old 512-slot allocations.
do
 local group={}
 for i=1,50 do group[i]=d.compile_mesh(rectangle(6+i%13),face) end
 collectgarbage('collect')
 local padding={};for i=1,256 do padding[i]=false end
 collectgarbage('stop')
 local i=0
 while p.vm_used(d.draw_mesh)<2*1024*1024-96*1024 do
  i=i+1;padding[i]=p.reserve(8192)
 end
 print('mesh cache tight quota used',p.vm_used(d.draw_mesh))
 for _,m in ipairs(group) do d.draw_mesh(m,cached) end
 padding=nil;collectgarbage('restart');collectgarbage('collect')
 for _,m in ipairs(group) do pixels(m) end
end
print('mesh cache sizing: small/large pixels, growth, shrink, overflow and OOM passed')
