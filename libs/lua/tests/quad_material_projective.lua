local d,p=require('display'),require('raster_test')
local colors={'red','white','blue','green'}
local palette=d.compile_palette(colors)
local entries={{.08,.92,1},{.2,.8,2,.05,.95},{.35,.65,3,.25,.75},
               {.45,.55,4,.45,.55}}
local material=d.compile_quad_material(d.compile_quad_batch(entries))
local immutable=p.mesh_snapshot(material)
local function draw(m,c,q,map,top,bottom)
 return d.draw_quad_material_projective(m,c,q[1],q[2],q[3],q[4],
  q[5],q[6],q[7],q[8],map[1],map[2],map[3],top,bottom)
end
local function plain(f,m,c,q,top,bottom)
 return f(m,c,q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],top,bottom)
end
-- Independent world-depth projection: interpolate endpoint depths, project
-- each layer endpoint through reciprocal depth, then recompile final cells.
local function project(u,map)
 if u<=map[1] then return 0 end
 if u>=map[2] then return 1 end
 local s=(u-map[1])/(map[2]-map[1])
 if map[3]==1 then return s end
 return (1/(1+s*(map[3]-1))-1)/(1/map[3]-1)
end
local function projected(recipe,map)
 local result={}
 for _,r in ipairs(recipe) do
  local a,b=math.max(r[1],map[1]),math.min(r[2],map[2])
  if a<b then
   result[#result+1]={project(a,map),project(b,map),r[3],r[4] or 0,r[5] or 1}
  end
 end
 return d.compile_quad_batch(result)
end
local quads={
 {3.17,2.13,58.37,8.19,48.31,58.71,7.23,53.29},
 {7.23,53.29,48.31,58.71,58.37,8.19,3.17,2.13},
 {-20.13,-12.19,50.23,5.11,54.31,47.17,-15.29,57.13},
 {8.13,5.17,51.37,5.17,43.41,54.29,13.27,54.29},
}
local maps={{0,1,1},{0,1,2},{.1,.8,2.3},{.2,.45,.25},{.55,1,8},
 {.8,.9,1e-4},{0,.2,1e4},{.35,.65,1},{.5,.5,2},{.999,1,1.25},
 {.5,.5+2^-52,2}}
for _,map in ipairs(maps) do
 local expected=d.compile_quad_material(projected(entries,map))
 for _,q in ipairs(quads) do
  d.clear('green');plain(d.draw_quad_material,expected,palette,q);d.present({retained=true})
  for _,c in ipairs({colors,palette}) do
   d.clear('green');assert(draw(material,c,q,map));assert(d.present()==0,'projected layers')
   d.clear('green')
   for bottom=240,1,-1 do assert(draw(material,c,q,map,bottom-1,bottom)) end
   assert(d.present()==0,'projective row composition')
   d.clear('blue');plain(d.draw_quad_material,expected,c,q,15,43);d.present()
   d.clear('blue');draw(material,c,q,map,15,43);assert(d.present()==0,'projective row membership')
   p.noalloc(function() draw(material,c,q,map,15,43) end)
   d.clear('green');plain(d.draw_quad_material,expected,palette,q);d.present()
  end
 end
end
-- Independent inverse bilinear/Newton oracle, then unproject depth to source U.
-- The production code has neither Newton iteration nor per-pixel UV inversion.
local function inverse_reference(q,map)
 local ex,ey=q[3]-q[1],q[4]-q[2]
 local dx,dy=q[7]-q[1],q[8]-q[2]
 local kx,ky=q[5]-q[7]-ex,q[6]-q[8]-ey
 for y=0,63 do for x=0,63 do
  local t,v=.5,.5
  for _=1,12 do
   local rx=q[1]+t*ex+v*dx+t*v*kx-x
   local ry=q[2]+t*ey+v*dy+t*v*ky-y
   local ax,ay=ex+v*kx,ey+v*ky
   local bx,by=dx+t*kx,dy+t*ky
   local det=ax*by-ay*bx
   if math.abs(det)<1e-12 then break end
   t=t-(rx*by-ry*bx)/det;v=v-(ax*ry-ay*rx)/det
  end
  if t>=0 and t<1 and v>=0 and v<1 then
   local s=map[3]==1 and t or (1/((1-t)+t/map[3])-1)/(map[3]-1)
   local u=map[1]+(map[2]-map[1])*s
   local owner
   for _,r in ipairs(entries) do
    if u>=r[1] and u<r[2] and v>=(r[4] or 0) and v<(r[5] or 1) then owner=r[3] end
   end
   if owner then d.fill_rect(x,y,1,1,colors[owner]) end
  end
 end end
end
for i=1,4 do
 local map=maps[i];local q=quads[i]
 d.clear('black');inverse_reference(q,map);d.present({retained=true})
 d.clear('black');draw(material,palette,q,map);assert(d.present()==0,'inverse projective oracle')
end
-- Adjacent source slices keep one continuous projective field. Project the
-- slice endpoints from the parent quad, rather than inventing new geometry.
for _,q in ipairs(quads) do
 local full={0,1,2.3}
 d.clear('black');draw(material,palette,q,full);d.present()
 d.clear('black')
 local boundaries={0,.2,.5,.8,1}
 for i=1,#boundaries-1 do
  local a,b=boundaries[i],boundaries[i+1]
  local ta,tb=project(a,full),project(b,full)
  local sub={}
  for axis=1,2 do
   sub[axis]=q[axis]+(q[axis+2]-q[axis])*ta
   sub[axis+2]=q[axis]+(q[axis+2]-q[axis])*tb
   sub[axis+4]=q[axis+6]+(q[axis+4]-q[axis+6])*tb
   sub[axis+6]=q[axis+6]+(q[axis+4]-q[axis+6])*ta
  end
  draw(material,palette,sub,{a,b,(1+b*1.3)/(1+a*1.3)})
 end
 assert(d.present()==0,'continuous source slices')
end
-- Clipped-out support is transparent; collapsed boundaries remain valid.
local sparse={{.1,.2,1,.1,.3},{.8,.9,2,.7,.9}}
local holes=d.compile_quad_material(d.compile_quad_batch(sparse))
for _,map in ipairs({{.3,.7,2},{0,.5,.5},{.5,1,2}}) do
 local expected=d.compile_quad_material(projected(sparse,map))
 d.clear('green');plain(d.draw_quad_material,expected,palette,quads[1]);d.present()
 d.clear('green');draw(holes,palette,quads[1],map);assert(d.present()==0,'source crop support')
end
-- Exact identity includes old fallback behavior and degenerate source records.
local legacy=d.compile_quad_material(d.compile_quad_batch({{.5,.5,1},{0,1,2}}))
for _,q in ipairs({quads[1],{0,0,64,0,64,64,0,64},{0,64,64,64,64,0,0,0},
 {2,2,55,55,2,55,55,2},{8,20,55,20,55,20,8,20}}) do
 d.clear('blue');local used=plain(d.draw_quad_material,legacy,palette,q);d.present()
 d.clear('blue');assert(draw(legacy,palette,q,{0,1,1})==used)
 assert(d.present()==0,'identity must preserve original API')
end
-- Fallback replays independently projected records, in the same painter order.
for _,q in ipairs({{2,2,55,55,2,55,55,2},{2,2,55,2,20,15,2,55},
 {8,20,55,20,55,20,8,20},{-100000,1,100000,1+1e-8,100000,2+1e-8,-100000,2}}) do
 for _,map in ipairs({{.125,.875,2},{.25,.75,.5}}) do
  local expected=projected(entries,map)
  for _,clip in ipairs({{0,240},{15,43}}) do
   d.clear('black');plain(d.draw_quad_batch,expected,palette,q,clip[1],clip[2]);d.present()
   d.clear('black');assert(not draw(material,palette,q,map,clip[1],clip[2]))
   assert(d.present()==0,'projected fallback')
   p.noalloc(function() draw(material,palette,q,map,clip[1],clip[2]) end)
  end
 end
end
-- Positive finite extreme ratios must remain bounded, including subnormals.
for _,ratio in ipairs({5e-324,1e-300,1e-20,1e20,1e300}) do
 local map={0,1,ratio}
 d.clear('black');draw(material,palette,quads[1],map);d.present()
 d.clear('black')
 for y=63,0,-1 do draw(material,palette,quads[1],map,y,y+1) end
 assert(d.present()==0,'extreme ratio composition')
end
-- Background damage and the complete pixel payload match a transformed field.
d.clear('green');local bg=d.capture_region(0,0,d.width,d.height)
d.restore_background(bg);d.present()
local map={.1,.8,2.3}
local expected=d.compile_quad_material(projected(entries,map))
plain(d.draw_quad_material,expected,palette,quads[1],5,50)
local a,b,c=p.display_snapshot(d.draw_quad_material,bg)
d.restore_background(bg);d.present();draw(material,palette,quads[1],map,5,50)
local x,y,z=p.display_snapshot(d.draw_quad_material_projective,bg)
assert(a==x and b==y and c==z,'projective pixels and damage')
d.restore_background(bg);assert(d.present()==0)
local function bad(...) assert(not pcall(...)) end
for _,map in ipairs({{-1,1,1},{0,2,1},{.8,.2,1},{0,1,0},{0,1,-1},
 {0,1,math.huge},{0,1,0/0},{0/0,1,1},{0,math.huge,1}}) do
 bad(draw,material,palette,quads[1],map,8,8);assert(d.present()==0)
end
bad(draw,material,{'red'},quads[1],{.5,.5,1},8,8)
bad(draw,material,palette,quads[1],{0,1,1},-1,240)
bad(draw,material,palette,quads[1],{0,1,1},0,241)
bad(draw,material,palette,quads[1],{0,1,1},20,19)
assert(d.present()==0)
local calls=0
local reentrant=setmetatable({}, {__index=function(_,key)
 calls=calls+1;draw(material,palette,quads[2],{.2,.6,.5},8,8)
 collectgarbage('collect');return key=='r' and 255 or 0
end})
local four_red={'red','red','red','red'}
d.clear('black');draw(material,four_red,quads[1],map);d.present()
d.clear('black');draw(material,{reentrant,reentrant,reentrant,reentrant},quads[1],map)
assert(calls==12 and d.present()==0,'projective reentry')
-- A 54-layer fixed profile with continuously changing geometry/crop/depth ratio.
local recipe,cs={},{}
for band=1,9 do for _,width in ipairs({.5,.4,.3,.2,.1,.05}) do
 local i=#recipe+1;recipe[i]={.5-width,.5+width,i,(band-1)/9,band/9}
 cs[i]={r=i*17%256,g=i*31%256,b=i*43%256}
end end
local batch=d.compile_quad_batch(recipe)
local profile=d.compile_quad_material(batch)
local cp=d.compile_palette(cs)
local moving={};local crop={0,1,1}
local shape={-100,-90,100,-90,85,90,-85,90}
local function place(i)
 local co,si=math.cos(i*.014),math.sin(i*.014)
 for j=1,8,2 do moving[j]=120+shape[j]*co-shape[j+1]*si;moving[j+1]=120+shape[j]*si+shape[j+1]*co end
 crop[1]=.01+(i%12)*.015;crop[2]=.8+(i%7)*.02;crop[3]=1.1+(i%15)*.07
end
local single={}
for i=1,6 do single[i]={recipe[i][1],recipe[i][2],i} end
local flat=d.compile_quad_material(d.compile_quad_batch(single))
for i=1,12 do
 place(i)
 for _,sample in ipairs({{flat,single},{profile,recipe}}) do
  local expected=d.compile_quad_material(projected(sample[2],crop))
  d.clear('black');plain(d.draw_quad_material,expected,cp,moving);d.present()
  d.clear('black');draw(sample[1],cp,moving,crop)
  assert(d.present()==0,'one/nine-band projected profile')
 end
end
assert(p.mesh_snapshot(material)==immutable,'draw mutated source material')
p.measure('projective_material_6',function(i) place(i);draw(flat,cp,moving,crop) end,1)
p.measure('projective_material_54',function(i) place(i);draw(profile,cp,moving,crop) end,1)
p.measure('projective_recompile_54',function(i)
 place(i);local compiled=d.compile_quad_material(projected(recipe,crop))
 plain(d.draw_quad_material,compiled,cp,moving)
end,3)
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(draw,material,{closing,closing,closing,closing},quads[1],{.5,.5,1})
