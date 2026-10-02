local d,p=require('display'),require('raster_test')
-- Captured projected quads: 24 poses, cropped nested six-layer fields,
-- one or nine transverse bands. No camera/game logic runs in this test.
local records=p.material_workload()
local definitions={}
for _,bands in ipairs({1,9}) do
 local entries={}
 for band=1,bands do
  for level,width in ipairs({.52,.40,.29,.20,.12,.065}) do
   local half=width/1.04
   entries[#entries+1]={.5-half,.5+half,9+(band-1)*6+level,(band-1)/bands,band/bands}
  end
 end
 definitions[bands*6]=entries
end
local function compile_fields()
 local fields={}
 for key,entries in pairs(definitions) do
  fields[key]=d.compile_quad_material(d.compile_quad_batch(entries))
 end
 return fields
end
local materials=compile_fields()
local colors={}
for i=1,64 do colors[i]={r=(i*37)%256,g=(i*71)%256,b=(i*113)%256} end
local palette=d.compile_palette(colors)
d.clear('black')
local region=d.capture_region(0,0,240,240)
local function draw(r,top,bottom)
 return d.draw_quad_material_projective(materials[r[1]],palette,
  r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],r[12],top,bottom)
end
local function reference(r,top,bottom)
 return p.material_reference(d.draw_quad_material_projective,materials[r[1]],palette,
  r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],r[12],top,bottom)
end
local function snapshot() return (p.display_snapshot(d.draw_quad_material,region)) end
local fallback=0
local function compare(r,top,bottom,label)
 d.clear('black');local fast=reference(r,top,bottom)
 local expected=snapshot()
 d.clear('black');assert(draw(r,top,bottom)==fast,'fallback selection '..label)
 if fast then assert(snapshot()==expected,'frozen Q24 pixel oracle '..label)
 else fallback=fallback+1 end
end
for i,r in ipairs(records) do
 compare(r,0,240,i..':full')
 compare(r,i%120,120+i%121,i..':rows')
end
-- Exact integer boundaries, positive/negative subpixel slopes and carries,
-- both winding orders, large offscreen coordinates, and extreme depth ratios.
for i=0,191 do
 local epsilon=({0,2^-25,-2^-25,2^-24,-2^-24,.5})[i%6+1]
 local left=i%2==0 and -99999 or -10+epsilon
 local right=i%3==0 and 99999 or 230+epsilon
 local shift=(i%7-3)/8+epsilon
 local r={i%2==0 and 6 or 54,left,-20,right+shift,10,
  right-shift,260,left+shift,240,0,1,({1,.125,8,1e-200,1e200})[i%5+1]}
 if i%4<2 then r[4],r[8]=r[8],r[4];r[5],r[9]=r[9],r[5] end
 compare(r,0,240,'boundary:'..i)
 compare(r,i%120,120+i%121,'boundary-rows:'..i)
end
-- Cropped support with transparent holes, horizontal/reversed edges and
-- multiple boundaries collapsing on either source endpoint.
materials[99]=d.compile_quad_material(d.compile_quad_batch({
 {.1,.9,1,.2,.8},{.2,.3,2,.35,.65},{.4,.6,3,.45,.55},{.7,.8,4,.1,.9}
}))
local knots={0,.1,.2,.3,.4,.6,.7,.8,.9,1}
local sparse=0
for i=1,#knots-1 do
 for j=i+1,#knots do
  for winding=0,3 do
   local r={99,-20,-10,260,10,230,230,0,240,knots[i],knots[j],
     ({1,.125,8,1e-200})[winding+1]}
   if winding>=2 then
    r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9]=-20,0,260,0,260,240,-20,240
   end
   if winding%2==1 then r[4],r[8]=r[8],r[4];r[5],r[9]=r[9],r[5] end
   compare(r,0,240,'sparse:'..i..':'..j..':'..winding)
   sparse=sparse+1
  end
 end
end
local dense={}
for i=0,30 do dense[#dense+1]={i/31,(i+1)/31,i+1} end
materials[100]=d.compile_quad_material(d.compile_quad_batch(dense))
for _,first in ipairs({0,15,30}) do
 for _,ratio in ipairs({1,1e-200,1e200}) do
  for reverse=0,1 do
   local r={100,-20,0,260,0,260,240,-20,240,first/31,(first+1)/31,ratio}
   if reverse==1 then r[4],r[8]=r[8],r[4];r[5],r[9]=r[9],r[5] end
   compare(r,0,240,'maximum-knots:'..first..':'..ratio..':'..reverse)
  end
 end
end
local groups={[6]={},[54]={}}
for _,r in ipairs(records) do
 groups[r[1]][#groups[r[1]]+1]=r
end
local function replay(rows)
 for _,r in ipairs(rows or records) do draw(r) end
end
p.noalloc(replay)
p.measure('material_workload_1074_calls_24_poses',function() replay() end,#records)
for _,key in ipairs({6,54}) do
 p.measure('material_workload_'..(key//6)..'_bands',function() replay(groups[key]) end,#groups[key])
end
-- Each sample's two materials have never drawn before. Compilation occurs
-- before timing; compare the same48 captured calls on a previously used pair.
local short={}
for _,key in ipairs({6,54}) do
 for i=1,24 do short[#short+1]=groups[key][i] end
end
local steady=materials
local fresh={}
for i=1,36 do fresh[i]=compile_fields() end
p.measure('material_fresh_pair_48_calls',function(i)
 materials=fresh[i+1];replay(short)
end,#short)
materials=steady
p.measure('material_warm_pair_48_calls',function() replay(short) end,#short)
print('material_workload: '..#records..' quads, full/row-clipped byte oracle and '..sparse..' sparse and 18 maximum-knot cases passed; fallbacks='..fallback)
