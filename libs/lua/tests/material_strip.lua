local d,p,v=require('display'),require('raster_test'),require('vmath')
p.oom(function() d.material_strip(1) end)
p.oom(function() d.material_strip(1) end,1)
local function buffer(t) local b=v.buffer(#t,'f64');b:load(t);return b end
local function rgb(c) return {r=(c>>11)*8,g=((c>>5)&63)*4,b=(c&31)*8} end
local materials={}
for _,bands in ipairs({1,9}) do
 local entries={}
 for band=1,bands do for level,width in ipairs({.52,.40,.29,.20,.12,.065}) do
  local half=width/1.04
  entries[#entries+1]={.5-half,.5+half,9+(band-1)*6+level,(band-1)/bands,band/bands}
 end end
 materials[bands*6]=d.compile_quad_material(d.compile_quad_batch(entries))
end
local bg=d.capture_region(0,0,240,240)
local function reset() d.restore_background(bg);d.present() end
local function snapshot() return p.display_snapshot(d.draw_material_strip,bg) end
local function equal(a,b,c,label)
 local x,y,z=snapshot();assert(a==x,label..' pixels');assert(b==y,label..' damage');assert(c==z,label..' dirty')
end
local function make(r)
 local g={a=r[3],b=r[4],t=r[5]>=0 and r[5] or nil,u=r[58],w=r[59],ratio=r[60],
  first={},last={},delta={},materials={},palettes={},indices={},colors={},visible={},ax={},bx={},lx={}}
 for i=1,13 do for axis=1,2 do
  local j=2*i-2+axis;g.first[j]=r[5+4*(i-1)+axis];g.last[j]=r[7+4*(i-1)+axis]
  g.delta[j]=g.last[j]-g.first[j]
 end end
 g.f,g.l=buffer(g.first),buffer(g.last)
 for i=1,12 do
  local at=60+(i-1)*67
  g.visible[i]=r[at+1]==1;g.materials[i]=materials[r[at+2]]
  local colors={};for j=1,64 do colors[j]=rgb(r[at+3+j]) end
  g.colors[i]=rgb(math.max(0,r[at+3]));assert(r[at+3]<0 or r[at+3]==r[at+67],'captured core palette index')
  g.palettes[i]=d.compile_palette(colors);g.indices[i]=r[at+3]>=0 and 64 or 0
 end
 g.strip=d.material_strip(12);g.strip:load(g.f,g.l,13);g.strip:bind(g.materials,g.palettes,g.indices)
 return g
end
local function native(g,top,bottom)
 return d.draw_material_strip(g.strip,g.a,g.b,g.u,g.w,g.ratio,g.t,top,bottom)
end
local function shared(g)
 for j=1,26 do
  g.ax[j]=g.first[j]+g.delta[j]*g.a;g.bx[j]=g.first[j]+g.delta[j]*g.b
  if g.t then g.lx[j]=math.floor(g.first[j]+g.delta[j]*g.t) end
 end
end
local function face(g,i,share,top,bottom,all)
 local j=2*i-1;local ax,ay,bx,by,cx,cy,dx,dy
 if share then
  ax,ay,bx,by=g.ax[j],g.ax[j+1],g.bx[j],g.bx[j+1]
  cx,cy,dx,dy=g.bx[j+2],g.bx[j+3],g.ax[j+2],g.ax[j+3]
 else
  local f,l=g.first,g.last
  ax,ay=f[j]+(l[j]-f[j])*g.a,f[j+1]+(l[j+1]-f[j+1])*g.a
  bx,by=f[j]+(l[j]-f[j])*g.b,f[j+1]+(l[j+1]-f[j+1])*g.b
  cx,cy=f[j+2]+(l[j+2]-f[j+2])*g.b,f[j+3]+(l[j+3]-f[j+3])*g.b
  dx,dy=f[j+2]+(l[j+2]-f[j+2])*g.a,f[j+3]+(l[j+3]-f[j+3])*g.a
 end
 local visible=math.max(ax,bx,cx,dx)>=-2 and math.min(ax,bx,cx,dx)<=242
  and math.max(ay,by,cy,dy)>=-2 and math.min(ay,by,cy,dy)<=242
 if not all and not visible then return 0,0 end
 local fast=d.draw_quad_material_projective(g.materials[i],g.palettes[i],ax,ay,bx,by,cx,cy,dx,dy,g.u,g.w,g.ratio,top,bottom)
 if g.t and g.indices[i]>0 then
  local x,y,X,Y
  if share then x,y,X,Y=g.lx[j],g.lx[j+1],g.lx[j+2],g.lx[j+3]
  else
   local f,l,t=g.first,g.last,g.t
   x,y=math.floor(f[j]+(l[j]-f[j])*t),math.floor(f[j+1]+(l[j+1]-f[j+1])*t)
   X,Y=math.floor(f[j+2]+(l[j+2]-f[j+2])*t),math.floor(f[j+3]+(l[j+3]-f[j+3])*t)
  end
  d.draw_line(x,y,X,Y,g.colors[i])
 end
 return fast and 1 or 0,fast and 0 or 1
end
local function scalar(g,share,all)
 if share then shared(g) end
 local fast,fallback=0,0
 for i=1,12 do local a,b=face(g,i,share,nil,nil,all);fast,fallback=fast+a,fallback+b end
 return fast,fallback
end
local poses={};local cases=0
-- Real projected rails and per-face colors captured from 24 deterministic
-- application poses. Geometry/style selection is fixture data, never native policy.
for pose=0,23 do
 local groups={};poses[#poses+1]=groups
 for _,r in ipairs(p.material_strip_workload(pose)) do groups[#groups+1]=make(r) end
 for _,g in ipairs(groups) do
  reset();local fast,fallback=scalar(g,false,true);local a,b,c=snapshot()
  reset();local x,y=native(g);assert(x==fast and y==fallback);equal(a,b,c,'interval')
  reset();scalar(g,true,true);equal(a,b,c,'Lua shared endpoint')
  -- Every individual face, including fallback and previously culled faces.
  for i=1,12 do
   local j=2*i-1;local one=d.material_strip(1)
   one:load(buffer({g.first[j],g.first[j+1],g.first[j+2],g.first[j+3]}),
            buffer({g.last[j],g.last[j+1],g.last[j+2],g.last[j+3]}),2)
   one:bind({g.materials[i]},{g.palettes[i]},{g.indices[i]})
   reset();local sf,sb=face(g,i,false,nil,nil,true);local a,b,c=snapshot()
   reset();local nf,nb=d.draw_material_strip(one,g.a,g.b,g.u,g.w,g.ratio,g.t)
   assert(sf==nf and sb==nb);equal(a,b,c,'face');cases=cases+1
  end
 end
 reset();for _,g in ipairs(groups) do scalar(g,false,false) end;local a,b,c=snapshot()
 reset();for _,g in ipairs(groups) do native(g) end;equal(a,b,c,'whole pose')
 collectgarbage('collect')
end
local function replay(mode)
 for _,groups in ipairs(poses) do for _,g in ipairs(groups) do
  if mode==0 then scalar(g,false,false) elseif mode==1 then scalar(g,true,false) else native(g) end
 end end
end
for mode=0,2 do
 p.noalloc(function() replay(mode) end)
 p.measure('strip_24_poses_'..({'scalar','Lua_shared','native'})[mode+1],function() replay(mode) end,mode==2 and 95 or 1612)
end
p.measure('strip_95_geometry_upload_load',function()
 for _,groups in ipairs(poses) do for _,g in ipairs(groups) do
  g.f:load(g.first);g.l:load(g.last);g.strip:load(g.f,g.l,13)
 end end
end,285)
p.measure('strip_95_bind',function()
 for _,groups in ipairs(poses) do for _,g in ipairs(groups) do g.strip:bind(g.materials,g.palettes,g.indices) end end
end,95)
p.measure('strip_24_poses_turning_upload_bind_draw',function()
 for _,groups in ipairs(poses) do for _,g in ipairs(groups) do
  g.f:load(g.first);g.l:load(g.last);g.strip:load(g.f,g.l,13)
  g.strip:bind(g.materials,g.palettes,g.indices);native(g)
 end end
end,475)
-- Dynamic blend arithmetic is timed separately with 116 captured visible lit
-- faces. Equal captured inputs preserve the oracle colors; this measures the
-- existing 64-entry blend call, not the consumer's color-selection recipe.
local blends={}
for _,groups in ipairs(poses) do for _,g in ipairs(groups) do for i=1,12 do
 if g.visible[i] and g.materials[i]==materials[54] then blends[#blends+1]=g.palettes[i] end
end end end
assert(#blends==116)
p.measure('strip_palette_blend_116_cost_probe',function()
 for _,palette in ipairs(blends) do d.blend_palette(palette,palette,palette,137) end
end,116)
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
-- No implicit first-draw cache. Measure a fresh constructor/load/bind/draw as
-- well as a cold draw on a freshly bound object outside the timed setup.
local g=poses[1][1]
p.measure('strip_cold_construct_load_bind_draw',function()
 local s=d.material_strip(12);s:load(g.f,g.l,13);s:bind(g.materials,g.palettes,g.indices)
 d.draw_material_strip(s,g.a,g.b,g.u,g.w,g.ratio,g.t)
end,4,true)
local fresh=d.material_strip(12);fresh:load(g.f,g.l,13);fresh:bind(g.materials,g.palettes,g.indices)
p.measure('strip_cold_draw',function() d.draw_material_strip(fresh,g.a,g.b,g.u,g.w,g.ratio,g.t) end,1,true)
p.measure('strip_hot_draw',function() native(g) end,1)
print('material strip real workload: '..cases..' faces, 95 intervals, 24 complete poses matched pixels/damage/order')
-- Bounded generic topology, validation before writes, transactional updates,
-- mutable palette identity and GC ownership. No consumer topology is assumed.
poses=nil;fresh=nil;g=nil;collectgarbage('collect')
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
