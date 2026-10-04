local d,p,v=require('display'),require('raster_test'),require('vmath')
p.oom(function() d.material_strip(1) end)
p.oom(function() d.material_strip(1) end,1)
local function buffer(t) local b=v.buffer(#t,'f64');b:load(t);return b end
local bg=d.capture_region(0,0,240,240)
local function reset() d.restore_background(bg);d.present() end
local function snapshot() return p.display_snapshot(d.draw_material_strip,bg) end
local function equal(a,b,c,label)
 local x,y,z=snapshot();assert(a==x,label..' pixels');assert(b==y,label..' damage');assert(c==z,label..' dirty')
end
-- One draw may reuse exact projective U values across distinct materials,
-- but equal knot counts alone are insufficient. V knots/owners may differ.
do
 local function material(entries) return d.compile_quad_material(d.compile_quad_batch(entries)) end
 local A=material({{.25,.75,1,.1,.9}})
 local B=material({{.25,.75,2,.3,.7}})
 local C=material({{.2,.8,1,.1,.9}})
 local D=material({{0,1,2}})
 local styles={A,B,C,A,D,D,B}
 local palette=d.compile_palette({'red','blue'})
 local palettes={};local first,last={},{}
 for i=1,8 do
  first[2*i-1],first[2*i]=-20+i%2*13,-30+(i-1)*43
  last[2*i-1],last[2*i]=250-i%2*7,first[2*i]+4
  if i<=7 then palettes[i]=palette end
 end
 local strip=d.material_strip(7)
 strip:load(buffer(first),buffer(last),8);strip:bind(styles,palettes)
 for _,q in ipairs({{0,1,0,1,1},{0,1,.1,.9,8},{.9,.1,.25,.75,.125},
                    {0,1,.2,.8,1e-200},{0,1,.2,.8,1e200},{0,1,.5,.5,2},
                    {.1,.9,.1,.9,2},{0,1,0,1,1}}) do
  reset();local fast,fallback=0,0
  for i=1,7 do
   local j=2*i-1
   local function xy(k,t) return first[k]+(last[k]-first[k])*t end
   local result=d.draw_quad_material_projective(styles[i],palette,
    xy(j,q[1]),xy(j+1,q[1]),xy(j,q[2]),xy(j+1,q[2]),
    xy(j+2,q[2]),xy(j+3,q[2]),xy(j+2,q[1]),xy(j+3,q[1]),q[3],q[4],q[5],37,193)
   fast=fast+(result and 1 or 0);fallback=fallback+(result and 0 or 1)
  end
  local a,b,c=snapshot();reset()
  local nf,nb=d.draw_material_strip(strip,q[1],q[2],q[3],q[4],q[5],nil,37,193)
  assert(nf==fast and nb==fallback,'mapped knot fallback selection')
  equal(a,b,c,'mapped knot reuse and new-call parameters')
 end
 p.noalloc(function() d.draw_material_strip(strip,0,1,.1,.9,8) end)
end
-- Empty faces must preserve scalar pixels, counts, lines and error preflight.
-- Generic nine-face strips deliberately alternate equal and unequal U knots;
-- none of this geometry or material selection depends on an application.
do
 local function material(entries) return d.compile_quad_material(d.compile_quad_batch(entries)) end
 local A=material({{.15,.85,1}})
 local B=material({{.15,.85,2,.2,.8}})
 local C=material({{.25,.75,1,.1,.9}})
 local E=material({})
 local palette=d.compile_palette({'red','blue'})
 local patterns={{A,A,A,B,B,C,C,A,A},{A,E,A,E,B,E,C,E,A},{E,E,E,E,E,E,E,E,E}}
 local groups={{},{},{}}
 local function render(g,q,top,bottom,scalar)
  if not scalar then
   return d.draw_material_strip(g.strip,q[1],q[2],q[3],q[4],q[5],q[6],top,bottom)
  end
  local fast,fallback=0,0
  for i=1,9 do
   local j=2*i-1
   local function xy(k,t) return g.first[k]+(g.last[k]-g.first[k])*t end
   local result=d.draw_quad_material_projective(g.styles[i],palette,
    xy(j,q[1]),xy(j+1,q[1]),xy(j,q[2]),xy(j+1,q[2]),
    xy(j+2,q[2]),xy(j+3,q[2]),xy(j+2,q[1]),xy(j+3,q[1]),q[3],q[4],q[5],top,bottom)
   fast=fast+(result and 1 or 0);fallback=fallback+(result and 0 or 1)
   if q[6] and g.lines[i]>0 then
    if top<bottom then d.draw_line(math.floor(xy(j,q[6])),math.floor(xy(j+1,q[6])),
     math.floor(xy(j+2,q[6])),math.floor(xy(j+3,q[6])),'blue') end
   end
  end
  return fast,fallback
 end
 local checks={{0,1,0,1,1,.5},{.9,.1,.125,.875,2,.5},{0,1,.5,.5,2,.5},
               {0,1,.2,.8,1e-200},{0,1,.2,.8,1e200},{0,0,.1,.9,2,0}}
 local fallback_count=0
 for pose=1,24 do
  local first,last={},{}
  for station=1,10 do
   local j=2*station-1
   first[j],first[j+1]=12+(pose+station)%7,8+(station-1)*24
   last[j],last[j+1]=225-(pose+station)%11,first[j+1]+pose%5-2
  end
  -- One self-crossing pose exercises nonempty fallback around empty faces.
  if pose==24 then last[2],last[4]=last[4],last[2] end
  for pattern,styles in ipairs(patterns) do
   local g={first=first,last=last,styles=styles,lines={},strip=d.material_strip(9)}
   local palettes={}
   for i=1,9 do palettes[i]=palette;g.lines[i]=i%3==0 and 2 or 0 end
   g.strip:load(buffer(first),buffer(last),10);g.strip:bind(styles,palettes,g.lines)
   groups[pattern][pose]=g
   for _,q in ipairs(checks) do for _,clip in ipairs({{0,240},{37,193},{91,91}}) do
    -- Scalar draw_line has no row clip; compare partial lines separately below.
    local line_t=q[6];if clip[1]==37 then q[6]=nil end
    reset();local sf,sb=render(g,q,clip[1],clip[2],true);local a,b,c=snapshot()
    reset();local nf,nb=render(g,q,clip[1],clip[2],false)
    assert(nf==sf and nb==sb and nf+nb==9,'empty face counts')
    equal(a,b,c,'empty face pixels/lines/damage/order');fallback_count=fallback_count+nb;q[6]=line_t
   end end
  end
 end
 assert(fallback_count>0,'empty workload includes fallback')
 local q={.1,.9,.125,.875,2,.5}
 for pattern=2,3 do
  local g=groups[pattern][1]
  reset();local nf,nb=render(g,q,0,240,false);local a,b,c=snapshot()
  reset()
  for row=0,239 do
   local rf,rb=render(g,q,row,row+1,false)
   assert(rf+rb==nf+nb,'row clips keep empty face counts')
  end
  equal(a,b,c,'empty face line row composition')
 end
 for pattern,group in ipairs(groups) do
  local function replay()
   for repeat_index=1,16 do for _,g in ipairs(group) do render(g,q,0,240,false) end end
  end
  p.noalloc(replay)
  p.measure('strip_generic_'..({'nonempty','mixed_empty','all_empty'})[pattern],replay,384)
 end
 local g=groups[2][1]
 local function bad(fn)
  reset();local a,b,c=snapshot();assert(not pcall(fn));equal(a,b,c,'empty preflight atomicity')
 end
 bad(function() d.draw_material_strip(g.strip,0/0,1,0,1,1) end)
 bad(function() d.draw_material_strip(g.strip,0,1,1,0,1) end)
 bad(function() d.draw_material_strip(g.strip,0,1,0,1,0) end)
 bad(function() d.draw_material_strip(g.strip,0,1,0,1,1,2) end)
 bad(function() d.draw_material_strip(g.strip,0,1,0,1,1,nil,-1,240) end)
 -- The final empty face's line endpoint must fail before the first A writes.
 local s=d.material_strip(2);local f=buffer({20,20,20,100,20,180})
 s:load(f,buffer({180,20,180,100,900,900}),3)
 s:bind({A,E},{palette,palette},{0,2})
 bad(function() d.draw_material_strip(s,0,1,0,1,1,1) end)
 bad(function() d.draw_material_strip(s,0,1,.5,.5,1,1,0,0) end)
 s:load(f,buffer({180,20,180,100,180,180}),3)
 reset();d.draw_material_strip(s,0,1,0,1,1,.5);local saved=(snapshot())
 bad(function() s:bind({A,E},{palette,palette},{0,3}) end)
 reset();d.draw_material_strip(s,0,1,0,1,1,.5);assert((snapshot())==saved,'failed empty bind unchanged')
 s:load(f,f,2);bad(function() d.draw_material_strip(s,0,1,0,1,1) end)
 print('material strip generic empty faces: 1296 scalar snapshot comparisons and noalloc passed')
end
-- Bounded generic topology, validation before writes, transactional updates,
-- mutable palette identity and GC ownership. No consumer topology is assumed.
collectgarbage('collect')
local red=d.compile_palette({'red'});local blue=d.compile_palette({'blue'})
local m=d.compile_quad_material(d.compile_quad_batch({{0,1,1}}))
local s=d.material_strip(2)
local f=buffer({20,20,20,100,20,180});local l=buffer({180,20,180,100,180,180})
s:load(f,l,3);s:bind({m,m},{red,blue},{1,1})
local function draw(t,top,bottom) return d.draw_material_strip(s,.1,.9,0,1,1,t,top,bottom) end
local function bad(fn)
 reset();local a,b,c=snapshot();assert(not pcall(fn));equal(a,b,c,'invalid input leaves framebuffer')
end
local invalids={
 function() s:load(buffer({0,0}),l,3) end,
 function() s:load(v.buffer(6,'f32'),l,3) end,
 function() s:load(f,l,1) end,
 function() s:load(f,l,4) end,
 function() s:load(buffer({20,20,20,100,100001,180}),l,3) end,
 function() s:bind({m,m},{red,d.compile_palette({})}) end,
 function() s:bind({m,m},{red,blue},{1,2}) end,
 function() s:bind({m},{red}) end,
 function() d.draw_material_strip(s,0/0,1,0,1,1) end,
 function() d.draw_material_strip(s,0,1,1,0,1) end,
 function() d.draw_material_strip(s,0,1,0,1,0) end,
 function() d.draw_material_strip(s,0,1,0,1,1,2) end,
 function() draw(nil,-1,240) end,
 function() draw(nil,100,99) end,
}
reset();draw(.5);local saved=(snapshot())
for _,fn in ipairs(invalids) do bad(fn);reset();draw(.5);assert((snapshot())==saved,'transactional state') end
local lookups=0
s:bind(setmetatable({m,m},{__index=function() lookups=lookups+1 end}),{red,blue})
assert(lookups==0);s:bind({m,m},{red,blue},{1,1})
-- A bad line endpoint at the final station must prevent earlier material writes,
-- including an empty clip/source. Disabled lines do not validate unused endpoints.
local remote=buffer({20,20,20,100,900,900})
s:load(f,remote,3)
bad(function() d.draw_material_strip(s,0,1,0,1,1,1) end)
bad(function() d.draw_material_strip(s,0,1,.5,.5,1,1,0,0) end)
d.draw_material_strip(s,0,1,0,1,1,nil)
s:load(f,l,3)
-- Original full-line phase is independently composed through half-open row clips.
for _,q in ipairs({{.1,.9,.5},{.9,.1,.5},{0,1,1},{0,0,0}}) do
 reset();d.draw_material_strip(s,q[1],q[2],.1,.9,2,q[3]);local a,b,c=snapshot()
 reset();for row=0,239 do d.draw_material_strip(s,q[1],q[2],.1,.9,2,q[3],row,row+1) end
 equal(a,b,c,'row composition')
end
-- Self-crossing faces and degenerate source records force full source replay.
local fallback=d.compile_quad_material(d.compile_quad_batch({{.5,.5,1},{0,1,1}}))
s:bind({fallback,m},{red,blue},{1,1})
local cross=buffer({180,100,180,20,180,180});s:load(f,cross,3)
local nf,nb=d.draw_material_strip(s,0,1,.125,.875,2,.5);assert(nb>0 and nf+nb==2)
reset();d.draw_quad_material_projective(fallback,red,20,20,180,100,180,20,20,100,.125,.875,2)
d.draw_line(100,60,100,60,'red')
d.draw_quad_material_projective(m,blue,20,100,180,20,180,180,20,180,.125,.875,2)
d.draw_line(100,60,100,180,'blue');local a,b,c=snapshot()
reset();d.draw_material_strip(s,0,1,.125,.875,2,.5);equal(a,b,c,'source fallback')
s:load(f,l,3);s:bind({m,m},{red,blue},{1,1})
-- Borrowed input buffers are copied; palettes are retained and read synchronously.
f:fill(0);l:fill(0);reset();draw(.5);assert((snapshot())==saved)
local output=d.compile_palette({'red'})
s:bind({m,m},{output,blue},{1,1});reset();draw(.5);assert((snapshot())==saved)
d.blend_palette(output,blue,blue,128);reset();draw(.5);assert((snapshot())~=saved)
s:bind({m,m},{output,output});reset();draw();local identical=(snapshot())
d.blend_palette(output,red,red,128);reset();draw();assert((snapshot())~=identical)
local weak=setmetatable({output},{__mode='v'});output=nil;collectgarbage('collect');assert(weak[1])
s:load(f,l,0);collectgarbage('collect');assert(weak[1]==nil,'count change releases palette roots')
assert(d.draw_material_strip(s,0,1,0,1,1)==0)
s:load(f,l,3);bad(function() draw() end);s:bind({m,m},{red,blue})
local mm,pp={m,m},{red,blue}
p.noalloc(function() s:load(f,l,3);s:bind(mm,pp);draw() end)
-- At t=1 cancellation must not become an endpoint shortcut; a fused
-- multiply-add would also move the second test's line entirely offscreen.
local exact=d.material_strip(1)
exact:bind({},{})
for _,q in ipairs({{1,-1e-17,1},{-1,2^-27,1-2^-27}}) do
 exact:load(buffer({q[1],10,q[1],100}),buffer({q[2],10,q[2],100}),2)
 exact:bind({m},{red},{1})
 local x=math.floor(q[1]+(q[2]-q[1])*q[3]);assert(x==0)
 reset();d.draw_line(x,10,x,100,'red');local a,b,c=snapshot()
 reset();d.draw_material_strip(exact,0,1,.5,.5,1,q[3]);equal(a,b,c,'binary64 rounding')
end
-- Constructor OOM leaves existing geometry/bindings usable, then recovers.
p.oom(function() d.material_strip(256) end);draw()
local many=d.material_strip(256);local xy={}
for i=1,257 do xy[#xy+1]=i%200;xy[#xy+1]=i%220 end
local large=buffer(xy);local mm,pp,ll={},{},{}
for i=1,256 do mm[i]=m;pp[i]=red;ll[i]=1 end
many:load(large,large,257);many:bind(mm,pp,ll)
p.noalloc(function() d.draw_material_strip(many,0,1,0,1,1,.5) end)
local empty=d.material_strip(0);empty:load(v.buffer(0,'f64'),v.buffer(0,'f64'),0);empty:bind({},{},{})
assert(d.draw_material_strip(empty,0,1,0,1,1,nil,0,0)==0)
d.deinit();assert(not pcall(d.draw_material_strip,empty,0,1,0,1,1))
s:load(f,l,0);s:bind({},{})
package.loaded.display=nil;d=require('display')
-- GC finalizer reentry during construction can close/reopen Display and create
-- another strip. Neither instance may borrow the old framebuffer or Lua arrays.
collectgarbage('collect');collectgarbage('incremental')
local pause=collectgarbage('param','pause',0)
local mul=collectgarbage('param','stepmul',0)
local step=collectgarbage('param','stepsize',0)
collectgarbage('stop')
local fired=false;local nested
local garbage=setmetatable({}, {__gc=function()
 assert(p.in_call(d.material_strip));fired=true
 d.deinit();nested=d.material_strip(1);nested:load(large,large,2);nested:bind({m},{red},{1})
end})
garbage=nil;collectgarbage('restart')
local outer=d.material_strip(2);assert(fired)
assert(not pcall(d.draw_material_strip,outer,0,1,0,1,1))
collectgarbage('param','pause',pause);collectgarbage('param','stepmul',mul)
collectgarbage('param','stepsize',step)
package.loaded.display=nil;d=require('display')
outer:load(large,large,3);outer:bind({m,m},{red,blue})
d.draw_material_strip(outer,0,1,0,1,1);d.draw_material_strip(nested,0,1,0,1,1,.5)
-- Incremental allocations for retained strip owners (already registered meta).
collectgarbage('collect');collectgarbage('stop')
local before=p.vm_used(d.draw_material_strip)
local owners={};for i=1,18 do owners[i]=false end
local baseline=p.vm_used(d.draw_material_strip)
for i=1,18 do owners[i]=d.material_strip(12) end
local after=p.vm_used(d.draw_material_strip)
print('material strip 18 owners retained bytes',after-baseline,'per owner',(after-baseline)/18,'owner table',baseline-before)
collectgarbage('restart')
print('material strip generic limits, atomic errors, zero hot allocations, OOM and finalizer reentry passed')
