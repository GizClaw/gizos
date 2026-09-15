local v,g = require('vmath'),require('geometry')
local function b(t) local x=v.buffer(#t); x:load(t); return x end
local function near(a,c,e) assert(math.abs(a-c)<(e or 1e-10),tostring(a)..' != '..tostring(c)) end
local seg=b{.1,.2,.3, -.2,.1,.4, .3,-.1,.2}
local weights=b{0,.3,1}; local axis=b{.6,.8,0}
local r=g.rotations(seg,weights,axis,3)
local endpoint,points=v.buffer(3),v.buffer(12)
local n=3
local function reference(angle,bend,yaw)
  local x,y,z=1,2,3
  for i=1,n do
    local a=angle-bend*weights:get(i)
    local ca,sa=math.cos(a),math.sin(a)
    local xx,yy,zz=seg:get(3*i-2),seg:get(3*i-1),seg:get(3*i)
    local ax,ay,az=axis:get(1),axis:get(2),axis:get(3)
    local dot=ax*xx+ay*yy+az*zz
    local dx=xx*ca+(ay*zz-az*yy)*sa+ax*dot*(1-ca)
    local dy=yy*ca+(az*xx-ax*zz)*sa+ay*dot*(1-ca)
    local dz=zz*ca+(ax*yy-ay*xx)*sa+az*dot*(1-ca)
    local cy,sy=math.cos(yaw),math.sin(yaw)
    x=x+dx*cy+dz*sy; y=y+dy; z=z-dx*sy+dz*cy
  end
  return x,y,z
end
for _,q in ipairs{0,1.6,-1.6,1.600001,4} do
  local fast=r:evaluate(endpoint,.4,q,.2,1,2,3,false,false)
  assert(fast==(math.abs(q)<=1.6))
  r:evaluate(points,.4,q,.2,1,2,3,true,false)
  local x,y,z=reference(.4,q,.2)
  near(endpoint:get(1),x,1e-9); near(endpoint:get(2),y,1e-9); near(endpoint:get(3),z,1e-9)
  for j=1,3 do near(endpoint:get(j),points:get(9+j),1e-9) end
end
r:evaluate(points,.4,0,.2,1,2,3,true,true)
local x,y,z=reference(.4,0,.2)
near(points:get(10),x);near(points:get(11),y);near(points:get(12),z)
assert(not pcall(function() r:evaluate(points,0,.1,0,0,0,0,true,true) end))
assert(not g.rotations(seg,b{0,2,1},axis,3):evaluate(endpoint,0,.1,0,0,0,0,false,false))
assert(not g.rotations(b{1e5,0,0},b{1},b{0,0,1},1):evaluate(endpoint,0,1,0,0,0,0,false,false))
assert(not pcall(g.rotations,seg,weights,b{0,0,0},3))
-- Different counts, signed weights and a deliberately nonunit axis exercise
-- the conservative moment domain without changing caller coefficients.
for _,count in ipairs{0,1,17,256} do
  n=count;seg=v.buffer(3*n);weights=v.buffer(n);axis=b{.6,.80001,0}
  for i=1,n do
    seg:set(3*i-2,.003*(i%3));seg:set(3*i-1,-.002);seg:set(3*i,.001)
    weights:set(i,(i%17)/8-1)
  end
  local prepared=g.rotations(seg,weights,axis,n);local out=v.buffer(3*(n+1))
  for _,angle in ipairs{0,16,16.001} do for _,bend in ipairs{-1.6,0,1.6,1.6001} do
    local fast=prepared:evaluate(out,angle,bend,.3,1,2,3,false,false)
    if math.abs(angle)>16 or math.abs(bend)>1.6 then assert(not fast) end
    local a,c,d=reference(angle,bend,.3)
    near(out:get(1),a,1e-9);near(out:get(2),c,1e-9);near(out:get(3),d,1e-9)
    prepared:evaluate(out,angle,bend,.3,1,2,3,true,false)
    near(out:get(3*n+1),a);near(out:get(3*n+2),c);near(out:get(3*n+3),d)
  end end
end
local invalid=g.rotations(b{1,0,0,1,0,0},b{0,0},b{0,0,1},2)
local held=v.buffer(9);held:fill(42)
assert(not pcall(function() invalid:evaluate(held,0,0,0,999999,0,0,true,false) end))
for i=1,9 do assert(held:get(i)==42) end
assert(not pcall(g.rotations,v.buffer(771),v.buffer(257),axis,257))
local base=b{1,1, 3,1, 3,3, 1,3, 0,4, 4,4}
local topology=b{0,1,4, 1,5,2}
local w0=b{0,1,2,3,4,5}; local w1=b{0,0,1,1,2,2}
local directions=b{0,1,1,0}
local batch=g.batch(base,topology,w0,w1,directions,6,2)
local pose=g.pose(batch);local values=v.buffer(18)
assert(not pcall(function() pose:copy(values) end))
local function evaluate() return pose:evaluate(.2,.1,1,2,.8,.6,.75,nil) end
assert(not evaluate()); local generation=pose:copy(values)
for i=1,6 do
  local xx=base:get(2*i-1)+w1:get(i)*.1
  local yy=base:get(2*i)+w0:get(i)*.2
  near(values:get(3*i-2),1+(xx*.8-yy*.6)*.75)
  near(values:get(3*i-1),2+(xx*.6+yy*.8)*.75)
  assert(values:get(3*i)==1)
end
assert(evaluate());assert(pose:copy(values)==generation)
-- Constructor storage is copied and the pose owns geometry after GC.
base:fill(0);w0:fill(0);w1:fill(0);topology:fill(0);directions:fill(0)
batch=nil;collectgarbage('collect');assert(evaluate())
local projection=b{0,1,1,0,10,.5,4}
assert(not pose:evaluate(.2,.1,1,2,.8,.6,.75,projection))
generation=pose:copy(values)
local saved={};for i=1,18 do saved[i]=values:get(i) end
projection:set(5,0)
assert(not pcall(function() pose:evaluate(.2,.1,1,2,.8,.6,.75,projection) end))
assert(pose:copy(values)==generation)
for i=1,18 do assert(values:get(i)==saved[i]) end
-- A zero denominator anywhere rejects the complete new pose; negatives are valid.
local plane=g.pose(g.batch(b{0,0,0,-1},b{1,1,2},nil,nil,b{0,0,0,0},2,1))
assert(not pcall(function() plane:evaluate(0,0,0,0,1,0,1,b{0,1,1,0,1,1,0}) end))
plane:evaluate(0,0,0,-2,1,0,1,b{0,1,1,0,1,1,0})
plane:copy(values);assert(values:get(3)<0)
assert(not pcall(g.batch,b{0,0},b{0,1,1},nil,nil,b{0,0,0,0},1,1))
local empty=g.pose(g.batch(b{},b{},nil,nil,b{0,0,0,0},0,0))
empty:evaluate(0,0,0,0,1,0,1,nil); empty:copy(v.buffer(0))
projection:set(5,10)
return function()
  r:evaluate(endpoint,.4,.2,.2,1,2,3,false,false)
  r:evaluate(points,.4,0,.2,1,2,3,true,true)
  pose:evaluate(.2,.1,1,2,.8,.6,.75,projection)
  pose:copy(values)
end
