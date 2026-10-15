local d,p,v=require('display'),require('raster_test'),require('vmath')
local function bad(...) assert(not pcall(...)) end
local bg
local function backdrop(frame)
 d.release_background();d.clear({r=frame*17%256,g=frame*31%256,b=frame*53%256})
 d.fill_rect(frame%160,frame%170,70,60,{r=123,g=71,b=211})
 bg=d.capture_region(0,0,d.width,d.height,nil,bg)
 d.restore_background(bg);d.present()
end
local function reset() d.restore_background(bg);d.present() end
local function snapshot() return {p.display_snapshot(d.stroke_path,bg)} end
local function same(a,b)
 assert(a[1]==b[1],'smooth RGB565')
 assert(a[2]==b[2],'smooth background tile mask')
 assert(a[3]==b[3],'smooth dirty bounds')
end
local function draw(points,widths,color,cache,offset,top,bottom,scale,tolerance,fast)
 return d.stroke_path(points,widths,color,offset or 0,top or 0,bottom or d.height,
                      cache,fast or false,true,scale or 1,tolerance or 0)
end
local function slot(widths)
 for _,value in pairs(widths) do
  if type(value)=='userdata' and getmetatable(value)=='display smooth coverage cache' then
   return value
  end
 end
end
local function compare(points,widths,color,offset,top,bottom,scale,tolerance)
 reset();assert(not draw(points,widths,color,false,offset,top,bottom,scale,tolerance))
 local expected=snapshot()
 reset();local hit=draw(points,widths,color,true,offset,top,bottom,scale,tolerance)
 same(expected,snapshot())
 reset();local warm=draw(points,widths,color,true,offset,top,bottom,scale,tolerance)
 same(expected,snapshot())
 return hit,warm
end
backdrop(1)
local pts={{20.25,78.125},{45.75,110.375},{60.25,78.125},{20.25,78.125}}
local widths={2.25,3.5,1.125}
local color={r=255,g=71,b=12}
local hit,warm=compare(pts,widths,color)
assert(not hit and warm)
local saved=slot(widths);local immutable=p.mesh_snapshot(saved)
for frame=1,48 do
 backdrop(frame)
 color.r,color.g,color.b=frame*19%256,frame*37%256,frame*61%256
 hit,warm=compare(pts,widths,color)
 assert(hit and warm,'color/background must not invalidate coverage')
 assert(slot(widths)==saved and p.mesh_snapshot(saved)==immutable)
end
p.noalloc(function() assert(draw(pts,widths,color,true)) end)
p.noalloc(function() assert(draw(pts,widths,'white',true,0,0,240,1,0,true)) end)

-- Content, not owner/point identity, decides hits. Every geometry/key mutation misses.
local function changed()
 local a,b=compare(pts,widths,color);assert(not a and b)
end
pts[2][1]=46.5;changed();widths[1]=4;changed()
pts={{20.25,78.125},{46.5,110.375},{60.25,78.125},{20.25,78.125}}
assert(draw(pts,widths,color,true))
for _,args in ipairs({{.5,0,240,1},{.5,76,110,1},{.5,76,110,.8}}) do
 local a,b=compare(pts,widths,color,table.unpack(args));assert(not a and b)
end
pts[4]=nil;widths[3]=nil;changed()
local xy=v.buffer(6);xy:load{20.25,78.125,46.5,110.375,60.25,78.125}
local descriptor={buffer=xy,count=3}
assert(draw(descriptor,widths,color,true))
xy:set(1,21);local a,b=compare(descriptor,widths,color);assert(not a and b)
local other=v.buffer(6);other:load{21,78.125,46.5,110.375,60.25,78.125}
descriptor.buffer=other;assert(draw(descriptor,widths,color,true))
local zeros,zero_width={{0,0},{0,0}},{0}
assert(not draw(zeros,zero_width,color,true));assert(draw(zeros,zero_width,color,true))
assert(not draw(zeros,zero_width,color,true,0,0,240,2),'scale participates even for zero geometry')

-- Unsupported combinations retain the old raster; do not accidentally read a hot slot.
local multicolor={'red','blue'}
for _,c in ipairs({multicolor,{'red','red'}}) do
 a,b=compare(pts,widths,c);assert(not a and not b)
end
a,b=compare(pts,widths,color,0,0,240,1,.1);assert(not a and not b)
-- Exact bbox/damage including degenerate and zero-alpha strokes, clipping,
-- tiny widths, float/double cutoff, crossings, and extreme/offscreen input.
for _,case in ipairs({
 {{{8,8},{8,8}},{0}}, {{{8,8},{8,8}},{.001}},
 {{{-100000,20},{100000,21}},{.01}},
 {{{4096,15},{20,30}},{2}}, {{{4096.001,15},{20,30}},{2}},
 {{{2.5,2.5},{5.5,5.5}},{1}}, {{{-9,-9},{-5,-5}},{1}},
 {{{8,8},{15,16},{8,16},{15,8}},{2,3,2}}
}) do
 for _,clip in ipairs({{0,240},{0,0},{15,16},{16,32},{240,240}}) do
  compare(case[1],case[2],color,0,clip[1],clip[2])
 end
end
-- Max point count and a small bounded clipped coverage map.
local many,thin={},{}
for i=1,256 do many[i]={i%7+1,i%5+1};if i<256 then thin[i]=.1 end end
a,b=compare(many,thin,color);assert(not a and b)

-- 16 KiB includes geometry key/header, not just alpha. Oversized draws fall
-- back without evicting the previous complete entry.
local huge={{120,20},{120,100}};local broad={1000}
a,b=compare(huge,broad,color,0,0,67);assert(not a and b)
local bounded=slot(broad)
assert(#p.mesh_snapshot(bounded)<=16384)
a,b=compare(huge,broad,color,0,0,68);assert(not a and not b)
assert(slot(broad)==bounded)
assert(draw(huge,broad,color,true,0,0,67))
local oversize={1000}
a,b=compare(huge,oversize,color);assert(not a and not b and not slot(oversize))

-- Preserve per-part glow/fill order with independent stable owners. A union
-- of all AA layers or a captured RGB565 image is not an equivalent reference.
local paths={{{20,70},{50,105},{65,70}},{{32,72},{70,95},{32,100}}}
local glow={{8,8},{7,7}};local ink={{2,2},{2.5,2.5}}
local glow_color={r=27,g=74,b=111}
local function layers(cache)
 for i=1,2 do
  draw(paths[i],glow[i],glow_color,cache)
  draw(paths[i],ink[i],color,cache)
 end
end
for frame=1,24 do
 backdrop(frame+80);color.r,color.b=frame*37%256,frame*17%256
 reset();layers(false);local expected=snapshot()
 reset();layers(true);same(expected,snapshot())
 reset();layers(true);same(expected,snapshot())
end
p.noalloc(function() layers(true) end)

-- Every input is validated on hits. Failure must not draw or corrupt old entries.
reset();draw(pts,widths,color,true);reset()
saved=slot(widths);immutable=p.mesh_snapshot(saved)
local clean=snapshot()
bad(draw,pts,widths,{r=256,g=0,b=0},true)
bad(draw,pts,widths,color,true,0,-1,240)
pts[1][1]=0/0;bad(draw,pts,widths,color,true);pts[1][1]=20.25
same(clean,snapshot());assert(slot(widths)==saved and p.mesh_snapshot(saved)==immutable)
local miss=function() draw(pts,widths,color,true,.75) end
p.oom(miss);same(clean,snapshot());assert(slot(widths)==saved)
assert(draw(pts,widths,color,true))
-- Fail separately at staging allocation and first owner-slot insertion.
for allowed=0,1 do
 local owner={2,3};local cold=function() return draw(pts,owner,color,true) end
 reset();clean=snapshot();p.oom(cold,allowed)
 same(clean,snapshot());assert(not slot(owner),'failed construction published a cache')
 assert(not cold());assert(slot(owner))
end

-- Getter reentry changes the same owner's cache; outer draw uses decoded
-- content, not a cache pointer acquired before callbacks/GC.
local recursive=setmetatable({}, {__index=function(_,key)
 draw({{5,5},{15,15},{20,5}},widths,'blue',true)
 collectgarbage('collect');reset()
 return key=='r' and 255 or 0
end})
reset();draw(pts,widths,'red',false);local expected=snapshot()
reset();draw(pts,widths,recursive,true);same(expected,snapshot())
assert(draw(pts,widths,'blue',true))

local function during_allocation(action,fn,native)
 collectgarbage('collect');collectgarbage('incremental')
 local pause=collectgarbage('param','pause',0)
 local mul=collectgarbage('param','stepmul',0)
 local step=collectgarbage('param','stepsize',0)
 collectgarbage('stop')
 local fired=false
 local garbage=setmetatable({}, {__gc=function()
  assert(p.in_call(native),'finalizer must run inside stroke binding')
  fired=true;action()
 end})
 garbage=nil;collectgarbage('restart');fn();assert(fired)
 collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
 collectgarbage('param','stepsize',step)
end
local function reopen()
 d.deinit();package.loaded.display=nil;d=require('display');backdrop(2)
end
for mode=1,3 do
 reopen();local owner={2,3};local native=d.stroke_path
 local function action()
  if mode==2 then d.deinit()
  elseif mode==3 then reopen()
  else
   draw({{5,5},{15,15},{20,5}},owner,'blue',true)
   -- Caller-owned table can lose private keys during a finalizer; publication
   -- must remain allocation-safe even when insertion is needed again.
   for key in pairs(owner) do if type(key)~='number' then owner[key]=nil end end
   reset()
  end
 end
 during_allocation(action,function()
  local ok=pcall(native,pts,owner,'red',0,0,240,true,false,true,1,0)
  assert(ok==(mode~=2),'allocation-driven Display lifecycle')
 end,native)
 if mode~=2 then
  local actual=snapshot();reset();draw(pts,owner,'red',false);same(actual,snapshot())
  assert(draw(pts,owner,'blue',true))
 end
end
-- Owner storage is ordinary VM data, with no registry root or raw framebuffer.
reopen()
do
 local owner={2,3};local cold=function() return draw(pts,owner,color,true) end
 reset();clean=snapshot()
 -- Staging succeeds, but the mandatory job scratch allocation fails.
 p.oom(cold,1);same(clean,snapshot());assert(not slot(owner))
 assert(not cold());assert(cold())
end
reopen();draw(pts,widths,color,false);draw(pts,widths,color,true)
collectgarbage('collect');local before=collectgarbage('count')
for i=1,80 do local owner={2,3};draw(pts,owner,color,true) end
collectgarbage('collect');assert(collectgarbage('count')<before+2,'cache owners leaked')
-- Cache survives display release while its owner is live; re-acquire validates
-- viewport/key and replays against the new framebuffer. Closed draws reject.
d.deinit();bad(draw,pts,widths,color,true)
package.loaded.display=nil;d=require('display');backdrop(3)
assert(draw(pts,widths,color,true))

-- Viewport dimensions invalidate even when geometry and clip remain identical.
draw(pts,widths,color,true,0,0,120)
assert(draw(pts,widths,color,true,0,0,120))
for _,size in ipairs({{160,240},{160,220},{240,240}}) do
 d.deinit();bg=nil;p.display_fixture_size(size[1],size[2])
 package.loaded.display=nil;d=require('display');backdrop(3)
 local cold,hot=compare(pts,widths,color,0,0,120)
 assert(not cold and hot,'viewport coverage key')
end

-- Host-only cost evidence; static positions and moving colors/background are
-- correctness-tested above. Never interpret these numbers as device FPS.
local owner={2,3};local plain=function() draw(pts,owner,color,false) end
plain()
collectgarbage('collect');local vm_before=collectgarbage('count')*1024
p.measure('smooth_cache_cold',function() draw(pts,owner,color,true) end,1,true)
collectgarbage('collect');local vm_charged=collectgarbage('count')*1024-vm_before
p.measure('smooth_cache_reference_64',function() for _=1,64 do plain() end end,64)
p.measure('smooth_cache_hot_64',function()
 for _=1,64 do assert(draw(pts,owner,color,true)) end
end,64)
print('smooth cache payload bytes',#p.mesh_snapshot(slot(owner)))
print('smooth cache retained VM bytes',vm_charged)
-- Exercise the real 2 MiB quota with owner-retained entries, then recover it.
collectgarbage('collect')
local baseline=collectgarbage('count')
local held={}
for i=1,2000 do held[i]=false end
local function allocate_owner()
 local w={2,3}
 draw(pts,w,color,true)
 return w
end
local exhausted=false
for i=1,2000 do
 local ok,value=pcall(allocate_owner)
 if not ok then assert(value=='not enough memory');exhausted=true;break end
 held[i]=value
end
assert(exhausted,'smooth coverage must be VM charged')
held=nil;collectgarbage('collect')
assert(collectgarbage('count')<baseline+2,'coverage owner quota recovery')
assert(draw(pts,owner,color,true))
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(draw,pts,owner,closing,true)
