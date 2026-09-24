local d,v,g=require('display'),require('vmath'),require('geometry')
local probe=require('raster_test')
local function b(t) local r=v.buffer(#t);r:load(t);return r end
if d.width==240 then
  local measure=probe.measure
  local xy,top,zero={}, {}, {}
  local polygons={}
  for i=0,47 do
    local x,y=(i%8)*28+2,math.floor(i/8)*28+2
    local p={{x,y},{x+20,y},{x+20,y+20},{x,y+20}};polygons[i+1]=p
    for _,q in ipairs(p) do xy[#xy+1]=q[1];xy[#xy+1]=q[2];zero[#zero+1]=0 end
    top[#top+1]=0;top[#top+1]=i*4+1;top[#top+1]=4
  end
  local base,topology,direction=b(xy),b(top),b{0,1,1,0}
  local pose,colors
  measure('prepared_geometry_construct',function()
    pose=g.pose(g.batch(base,topology,nil,nil,direction,192,48))
    colors=v.buffer(48);colors:fill(0xf800)
  end,4,true)
  local function evaluate(i) pose:evaluate(0,0,(i or 0)%2,0,1,0,1,nil) end
  measure('prepared_pose_first',evaluate,1,true)
  measure('prepared_pose_changed',evaluate,1)
  evaluate(0)
  measure('prepared_pose_reused',function() evaluate(0) end,1)
  local function draw() d.draw_pose(pose,colors,0,0,0,0,1,0,0,0,240,240) end
  local function scalar() for _,p in ipairs(polygons) do d.fill_polygon(p,'red') end end
  measure('prepared_draw_first',draw,1,true)
  d.clear('black');scalar();d.present({retained=true});d.clear('black');draw();assert(d.present()==0)
  measure('prepared_draw_warm',draw,1)
  measure('prepared_scalar_polygon_reference',scalar,48)
  local points={};for i=0,255 do points[#points+1]=(i%64)-32;points[#points+1]=i%2==0 and -10 or 10;points[#points+1]=2 end
  local input,camera=b(points),b{120,100,3,0,1}
  local chain,style
  measure('prepared_polyline_construct',function()
    chain=d.polyline(256);chain:load(input,256)
    style=d.compile_line_style(b{255,0,0,0,0,255,0,.01,.5,0,1},2,'mean','multiply')
  end,5,true)
  local function lines() d.draw_polyline(chain,camera,2,0,false,style,style,style,0,0,240,240) end
  measure('prepared_polyline_first',lines,1,true)
  measure('prepared_polyline_warm',lines,1)
  d.deinit();return
end
local function bad(f,...) assert(not pcall(f,...)) end
local function equal(reference,actual,label)
  d.clear('black');reference();d.present({retained=true})
  d.clear('black');actual();assert(d.present()==0,label)
end
local function rgb(c) return {r=((c>>11)&31)<<3,g=((c>>5)&63)<<2,b=(c&31)<<3} end
local function line(a,c,color,offset,top,bottom)
  local x,y=a[1]+(offset or 0),a[2]
  local dx,dy=c[1]-a[1],c[2]-a[2]
  local lo,hi=0,1;top,bottom=top or 0,bottom or 8
  for _,q in ipairs{{x,dx,0,7},{y,dy,top,bottom-1}} do
    if q[2]==0 then if q[1]<q[3] or q[1]>q[4] then return end
    else
      local u,w=(q[3]-q[1])/q[2],(q[4]-q[1])/q[2]
      lo=math.max(lo,math.min(u,w));hi=math.min(hi,math.max(u,w))
    end
  end
  if lo>hi then return end
  d.draw_line(math.floor(x+dx*lo+.5),math.floor(y+dy*lo+.5),
    math.floor(x+dx*hi+.5),math.floor(y+dy*hi+.5),rgb(color))
end
local xy={1,1, 4,1, 4,3, 1,3, 1,5, 5,5}
local w0,w1={0,1,1,0,0,1},{0,0,1,1,0,1}
local pose=g.pose(g.batch(b(xy),b{0,1,4,1,5,2},b(w0),b(w1),b{0,1,1,0},6,2))
local colors=b{0xf800,0x07e0}
local proj=b{0,1,1,0,8,.5,5}
local copied=v.buffer(18)
local function reference(projected,layer,scale,offset,tint)
  local points={}
  for i=1,6 do
    local x=xy[2*i-1]+w1[i]*.2;local y=xy[2*i]+w0[i]*.3
    local u=(x*.8-y*.6)*.75+2;local z=(x*.6+y*.8)*.75+.5
    if projected then
      local r=1/(1+z/8)
      u=((1+u*r)+.25);z=(5-(z*.5)*r)-layer*r-.2
    end
    points[i]={u*scale,z*scale}
  end
  d.fill_polygon({points[1],points[2],points[3],points[4]},rgb(tint or 0xf800),offset,0,8)
  line(points[5],points[6],tint or 0x07e0,offset)
end
for _,projected in ipairs{false,true} do
  pose:evaluate(.3,.2,2,.5,.8,.6,.75,projected and proj or nil)
  local generation=pose:copy(copied)
  for _,layer in ipairs{0,.6,1.2} do
    for _,scale in ipairs{1,.8} do
      for _,offset in ipairs{0,.49,.51} do
        local function draw() d.draw_pose(pose,colors,1,.25,-.2,layer,scale,offset,0,0,8,8) end
        equal(function() reference(projected,layer,scale,offset) end,draw,'pose pixels')
        draw();assert(d.present()==0,'warm pose')
        assert(pose:copy(copied)==generation,'layer changed base pose')
      end
    end
  end
end
local function draw_pose() d.draw_pose(pose,colors,1,.25,-.2,.6,1,0,0,0,8,8) end
colors:set(2,-1);bad(draw_pose);assert(d.present()==0,'late color changed frame');colors:set(2,0x07e0)
equal(function() reference(true,.6,1,0,0xffff) end,
  function() d.draw_pose(pose,colors,1,.25,-.2,.6,1,0,0,0,8,8,'white') end,'tint')
-- The oracle uses original endpoints for color, strict signs for split, then near clipping.
local camera=b{4,3,2,0,1}
local points={{-2,-1,1},{2,1,2},{0,0,.5},{-1,0,1},{1,0,2},{-1,-1,2}}
local channel={0,.2,.9,.6,1,.1}
local function packed()
  local p,c={},{}
  for i,a in ipairs(points) do for j=1,3 do p[#p+1]=a[j];c[#c+1]=channel[i] end end
  return b(p),b(c)
end
local input,channels=packed();local batch=d.polyline(#points);batch:load(input,#points,channels)
local descriptors={
  {v={255,3,19, 7,251,233, -.2,.75,.1,0,1},axis=2,op='multiply',reducer='mean'},
  {v={0,255,0, 255,0,255, .1,1.3,0,.1,.9},axis=4,op='divide',reducer='min'},
}
local fixed={0x001f,0xffff,0xf800,0x07e0,0xffe0}
local styles={}
for i,a in ipairs(descriptors) do styles[i]=d.compile_line_style(b(a.v),a.axis,a.reducer,a.op) end
styles[3]=d.compile_line_style(b(fixed))
local function color(style,source)
  if style==3 then return fixed[source] end
  local a=descriptors[style];local j=a.axis
  local x=j<=3 and points[source][j] or channel[source]
  local y=j<=3 and points[source+1][j] or channel[source+1]
  local value=a.reducer=='mean' and (x+y)*.5 or math.min(x,y)
  local q=a.v;local t=value-q[7]
  t=(a.op=='multiply' and t*q[8] or t/q[8])+q[9]
  t=math.max(q[10],math.min(q[11],t))
  local r=math.floor(q[1]*(1-t)+q[4]*t)
  local g0=math.floor(q[2]*(1-t)+q[5]*t)
  local bb=math.floor(q[3]*(1-t)+q[6]*t)
  return ((r>>3)<<11)|((g0>>2)<<5)|(bb>>3)
end
local function fragment(a,c,ink)
  local near=camera:get(5)
  if a[3]<near and c[3]<near then return end
  local aa,cc={table.unpack(a)},{table.unpack(c)}
  if aa[3]<near or cc[3]<near then
    local t=(near-aa[3])/(cc[3]-aa[3])
    local cut={aa[1]+(cc[1]-aa[1])*t,aa[2]+(cc[2]-aa[2])*t,near}
    if aa[3]<near then aa=cut else cc=cut end
  end
  local function project(p)
    return {camera:get(1)+camera:get(3)*p[1]/p[3],camera:get(2)+camera:get(3)*(camera:get(4)-p[2])/p[3]}
  end
  line(project(aa),project(cc),ink)
end
local function oracle(axis,offset,reverse)
  for k=1,#points-1 do
    local i=reverse and #points-k or k;local a,c=points[i],points[i+1]
    local sa,sb=a[axis]-offset,c[axis]-offset
    if sa*sb<0 then
      local t=(offset-a[axis])/(c[axis]-a[axis]);local cut={}
      for j=1,3 do cut[j]=a[j]+t*(c[j]-a[j]) end;cut[axis]=offset
      fragment(a,cut,color(sa<0 and 2 or 1,i));fragment(cut,c,color(sb<0 and 2 or 1,i))
    else fragment(a,c,color((sa<0 and sb<0) and 2 or ((sa>0 and sb>0) and 1 or 3),i)) end
  end
end
for axis=1,3 do for _,offset in ipairs{0,.5,1} do for _,reverse in ipairs{false,true} do
  local function draw() d.draw_polyline(batch,camera,axis,offset,reverse,styles[1],styles[2],styles[3],0,0,8,8) end
  equal(function() oracle(axis,offset,reverse) end,draw,'polyline pixels '..axis..'/'..offset..'/'..tostring(reverse))
  draw();assert(d.present()==0,'warm polyline')
end end end
local function draw_line() d.draw_polyline(batch,camera,2,0,false,styles[1],styles[2],styles[3],0,0,8,8) end
-- Camera, copied points and newly compiled styles cannot reuse stale projections/colors.
camera:set(1,3);equal(function() oracle(2,0,false) end,draw_line,'camera change')
points[6][2]=.75;input,channels=packed();batch:load(input,#points,channels)
equal(function() oracle(2,0,false) end,draw_line,'point reload')
input:fill(0);channels:fill(0);collectgarbage('collect')
equal(function() oracle(2,0,false) end,draw_line,'owned storage')
local short=d.compile_line_style(b{1})
bad(d.draw_polyline,batch,camera,2,0,false,styles[1],short,styles[3],0,0,8,8)
assert(d.present()==0,'late style changed pixels')
bad(batch.load,batch,v.buffer(2),#points);draw_line();assert(d.present()==0)
local overflow=d.compile_line_style(b{0,0,0,255,255,255,0,1e-320,0,0,1},2,'mean','divide')
bad(d.draw_polyline,batch,camera,2,0,false,styles[1],styles[2],overflow,0,0,0,0)
assert(d.present()==0,'invalid hidden gradient')
local empty=d.polyline(0);local no=d.compile_line_style(b{})
d.draw_polyline(empty,camera,1,0,false,no,no,no,0,0,8,8);assert(d.present()==0)
bad(d.polyline,257);bad(d.compile_line_style,b{0,0,0,255,255,256,0,1,0,0,1},1,'mean','multiply')
-- Maximum extent creates all 510 crossing fragments without growing storage.
local maximum=d.polyline(256);local many=v.buffer(768)
for i=1,256 do many:set(3*i-2,0);many:set(3*i-1,i%2==0 and 1 or -1);many:set(3*i,2) end
maximum:load(many,256)
equal(function() line({3,4},{3,2},color(1,1)) end,
  function() d.draw_polyline(maximum,camera,2,0,false,styles[1],styles[1],styles[1],0,0,8,8) end,'maximum fragments')
maximum,many=nil,nil;collectgarbage('collect')
local before_clip=function() d.draw_pose(pose,colors,0,0,0,0,1,0,0,0,0,0) end
before_clip();assert(d.present()==0)
-- Clip and damage follow the same framebuffer bookkeeping as existing drawing.
d.clear('white');d.present();local bg=d.capture_region(0,0,8,8)
d.restore_background(bg);d.present();draw_pose();draw_line();d.restore_background(bg);assert(d.present()==0)
probe.noalloc(function() draw_pose();draw_line() end)
probe.oom(function() return d.polyline(8) end)
probe.oom(function() return g.pose(g.batch(b{0,0,1,1},b{1,1,2},nil,nil,b{0,0,0,0},2,1)) end)
local closing=setmetatable({}, {__index=function() d.deinit();return 0 end})
bad(d.draw_pose,pose,colors,0,0,0,0,1,0,0,0,8,8,closing)
bad(draw_line)
