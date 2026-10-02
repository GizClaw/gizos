local d,p=require('display'),require('raster_test')
-- Captured projected quads: 24 poses, cropped nested six-layer fields,
-- one or nine transverse bands. No camera/game logic runs in this test.
local records=p.material_workload()
local materials={}
for _,bands in ipairs({1,9}) do
 local entries={}
 for band=1,bands do
  for level,width in ipairs({.52,.40,.29,.20,.12,.065}) do
   local half=width/1.04
   entries[#entries+1]={.5-half,.5+half,9+(band-1)*6+level,(band-1)/bands,band/bands}
  end
 end
 materials[bands*6]=d.compile_quad_material(d.compile_quad_batch(entries))
end
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
local function replay()
 for _,r in ipairs(records) do draw(r) end
end
p.noalloc(replay)
p.measure('material_workload_1074_calls_24_poses',replay,#records)
print('material_workload: '..#records..' quads, full/row-clipped byte oracle passed; fallbacks='..fallback)
