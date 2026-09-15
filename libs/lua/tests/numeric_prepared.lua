local v = require('vmath')
local function b(t) local x=v.buffer(#t); x:load(t); return x end
local function near(a,c,e) assert(math.abs(a-c) <= (e or 1e-10), tostring(a)..' != '..tostring(c)) end
local p=b{0,0,0, 1.000000001,0,0, 2.1,0,0}
local prev=b{0,0,0, 1,0,0, 2,0,0}
local edges=b{1,2,1,.0001,0,2, 2,3,.8,.0002,3,1}
local w=v.constraints(3,2)
local out,old,lambda=v.buffer(9),v.buffer(9),v.buffer(2)
local mobility=b{0,1,1}; local before=b{0,0,0, 0,0,0, .01,0,0}
local gain0=b{1,1,1, .5,.5,.5, .25,.25,.25}
local gain1=b{1,1,1, .5,.5,.5, .5,.5,.5}
local after=b{0,0,0, .01,0,0, 0,0,0}
local bounds=b{2,2,2,0,0}
local function reset() w:load(p,prev,edges,3,2,.01) end
local function snapshot() return w:copy(out,old,lambda) end
local function fail(f)
  local span=snapshot()
  local saved={}
  for i=1,9 do saved[i]=out:get(i); saved[i+9]=old:get(i) end
  local la,lb=lambda:get(1),lambda:get(2)
  assert(not pcall(f))
  assert(snapshot()==span)
  for i=1,9 do assert(out:get(i)==saved[i]); assert(old:get(i)==saved[i+9]) end
  assert(lambda:get(1)==la and lambda:get(2)==lb)
end
assert(not pcall(v.constraints,257,2)); assert(not pcall(v.constraints,2,513))
assert(not pcall(v.constraints,0,0)); assert(not pcall(v.constraints,2.5,0))
assert(not pcall(function() w:solve(1,nil,0) end))
reset()
w:integrate(1,3,mobility,before,gain0,gain1,after,'f64',bounds,1)
snapshot()
near(out:get(1),0); near(old:get(4),p:get(4))
near(out:get(4),(p:get(4)+(p:get(4)-prev:get(4))*.5*.5)+.01)
near(out:get(7),(p:get(7)+((p:get(7)-prev:get(7))+.01)*.25*.5))
w:node(3,2.2,0,0,2.1,0,0)
local x,y,z,px,py,pz=w:node(3)
near(x,2.2); near(px,2.1); assert(y==0 and z==0 and py==0 and pz==0)
w:span(1,3,1.8,.0001,0,1)
w:solve(4,bounds,1)
local sl=snapshot(); assert(sl<0 and lambda:get(1)<=0 and lambda:get(2)<=0)
local retained=lambda:get(1)
w:solve(1,bounds,1); snapshot(); assert(lambda:get(1)~=0 and retained~=0)
w:begin(.02); assert(snapshot()==0 and lambda:get(1)==0 and lambda:get(2)==0)
w:edge(1,1,2,.9,.0002,0,2); w:span(nil)
w:solve(2,nil,0); w:damp(mobility,.25,.4,.998,1e-8)
fail(function() w:solve(0,nil,0) end)
fail(function() w:solve(33,nil,0) end)
fail(function() w:solve(1,b{1,3,2,0,-1},1) end)
fail(function() w:span(1,4,1,0,0,1) end)
fail(function() w:node(1,0,0,0,0,0,0/0) end)
fail(function() w:edge(2,2,3,.8,.0002,3,-1) end)
fail(function() w:begin(0) end)
fail(function() w:load(p,p,edges,3,2,.01) end)
fail(function() w:load(p,prev,b{1,2,1,0,0,1, 2,4,1,0,1,1},3,2,.01) end)
fail(function() w:integrate(1,3,mobility,before,gain0,gain0,after,'f64',nil,0) end)
fail(function() w:integrate(1,3,mobility,before,gain0,gain1,after,'float',nil,0) end)
fail(function() w:damp(mobility,1.1,0,0,0) end)
fail(function() w:copy(out,out,lambda) end)
fail(function() w:copy(v.buffer(9,'f32'),old,lambda) end)
-- Compare one multi-iteration call with repeated solves retaining lambda/order.
local w2=v.constraints(3,2); w2:load(p,prev,edges,3,2,.01)
reset(); w:solve(4,bounds,1); snapshot()
w2:solve(2,bounds,1); w2:solve(2,bounds,1)
local p2,l2,o2=v.buffer(9),v.buffer(2),v.buffer(9); w2:copy(p2,o2,l2)
for i=1,9 do near(p2:get(i),out:get(i),1e-14) end
for i=1,2 do near(l2:get(i),lambda:get(i),1e-14) end
-- A sub-float strain must not disappear when coordinates near one are rounded.
local taut=v.constraints(2,1)
taut:load(b{0,0,0,1.000000001,0,0},b{0,0,0,1.000000001,0,0},b{1,2,1,.0001,0,1},2,1,.01)
taut:solve(1,nil,0); taut:copy(out,old,lambda)
assert(lambda:get(1)<0 and math.abs(lambda:get(1))<1e-8)
-- Empty edge sets and pinned mobility do not invent topology or movement.
local empty=v.constraints(1,0); empty:load(b{1,2,3},b{0,1,2},b{},1,0,.01)
empty:solve(32,nil,0); empty:damp(b{0},1,1,1,1e-8)
empty:copy(out,old,lambda); assert(out:get(1)==1 and old:get(1)==0)
-- Exceptional magnitudes preserve compensated state or fail without partial writes.
local large=v.constraints(2,1)
large:load(b{999999,0,0,1000000,0,0},b{999999,0,0,1000000,0,0},b{1,2,.999999999,.0001,0,1},2,1,.01)
large:solve(1,nil,0);large:copy(out,old,lambda);assert(lambda:get(1)<0)
local tiny=v.constraints(2,1)
tiny:load(b{0,0,0,1e-12,0,0},b{0,0,0,1e-12,0,0},b{1,2,0,0,0,1},2,1,.01)
tiny:span(1,2,0,0,0,1);tiny:solve(1,nil,0);tiny:copy(out,old,lambda)
near(out:get(4),0,1e-25) -- squared distance below the refined-sqrt seed domain.
reset();w:node(3,1e6,0,0,-1e6,0,0)
fail(function() w:integrate(1,3,mobility,before,gain0,gain1,after,'f64',nil,0) end)
-- A non-finite float coefficient product must not be hidden by fmin/bounds.
w:edge(1,1,2,1,0,0,1e-320)
fail(function() w:solve(1,bounds,1) end)
-- Coefficient invalidation is compared against independently loaded state.
reset();w:begin(.02);w:edge(2,2,3,.7,.0003,2,1)
w2:load(p,prev,b{1,2,1,.0001,0,2,2,3,.7,.0003,2,1},3,2,.02)
w:solve(3,nil,0);w2:solve(3,nil,0);snapshot();w2:copy(p2,o2,l2)
for i=1,9 do assert(p2:get(i)==out:get(i)) end
for i=1,2 do assert(l2:get(i)==lambda:get(i)) end
local free=v.constraints(2,0)
free:load(b{0,0,0,2,0,0},b{0,0,0,2,0,0},b{},2,0,.01)
free:span(1,2,1,.0001,1,3);free:solve(1,nil,0)
near(free:copy(out,old,lambda),-.2);near(out:get(1),.2);near(out:get(4),1.4)
assert(old:get(1)==0 and old:get(4)==2)
free:span(nil);free:solve(1,b{1,2,2,0,3,1,2,2,4,5},2)
free:copy(out,old,lambda);assert(out:get(2)==4 and out:get(5)==4)
-- Returned closure is checked by the C harness for successful warm allocation.
return function()
  reset()
  w:begin(.01)
  w:integrate(1,3,mobility,before,gain0,gain1,after,'displacement-f32',bounds,1)
  w:node(1,0,0,0,0,0,0)
  w:edge(1,1,2,1,.0001,0,2)
  w:span(1,3,1.8,.0001,0,1)
  w:solve(6,bounds,1)
  w:damp(mobility,.25,.4,.998,1e-8)
  w:copy(out,old,lambda)
end
