local d,p,v=require('display'),require('raster_test'),require('vmath')
local function buffer(t) local b=v.buffer(#t,'f64');b:load(t);return b end
local bg=d.capture_region(0,0,240,240)
local function reset() d.restore_background(bg);d.present() end
local function snapshot() return p.display_snapshot(d.draw_material_strip,bg) end
local function equal(a,b,c,label)
 local x,y,z=snapshot();assert(a==x,label..' pixels');assert(b==y,label..' damage');assert(c==z,label..' dirty')
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
