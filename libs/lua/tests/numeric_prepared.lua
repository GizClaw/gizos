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
local bound=v.constraints(3,2)
local bp,bprev,be=v.buffer(12),v.buffer(12),v.buffer(12)
bp:fill(91);bprev:fill(92);bp:copy(p,1,1,9);bprev:copy(prev,1,1,9);be:copy(edges,1,1,12)
bound:bind(bp,bprev,be,3,2,.01)
do
 local function reject(fn)
  local span=bound:copy(out,old,lambda)
  local saved,ls={},{}
  for i=1,12 do saved[i]=bp:get(i);saved[i+12]=bprev:get(i) end
  for i=1,2 do ls[i]=lambda:get(i) end
  assert(not pcall(fn))
  assert(bound:copy(out,old,lambda)==span)
  for i=1,12 do assert(bp:get(i)==saved[i] and bprev:get(i)==saved[i+12]) end
  for i=1,2 do assert(lambda:get(i)==ls[i]) end
 end
 bp:set(4,1.3);assert(bound:node(2)==1.3)
 bound:node(2,1.4,.2,.3,1.2,.1,.2)
 assert(bp:get(4)==1.4 and bp:get(5)==.2 and bprev:get(4)==1.2)
 bound:span(1,3,1.8,.0001,0,1);bound:solve(1,nil,0)
 local span=bound:copy(out,old,lambda)
 local edge,sl=bound:multipliers(1);assert(edge==lambda:get(1) and sl==span)
 assert(select(2,bound:multipliers())==span)
 bound:span(nil);assert(select(2,bound:multipliers(nil))==span)
 local held=edge;bp:set(1,0);assert(bound:multipliers(1)==held)
 reject(function() bound:multipliers(0) end)
 reject(function() bound:multipliers(3) end)
 reject(function() bound:multipliers(1.5) end)
 local competitor=v.constraints(3,2)
 reject(function() competitor:bind(bp,bprev,be,3,2,.01) end)
 reject(function() competitor:bind(bprev,bp,be,3,2,.01) end)
 reject(function() bound:bind(bp,bp,be,3,2,.01) end)
 reject(function() bound:bind(bp,bprev,be,4,2,.01) end)
 reject(function() bound:bind(v.buffer(9,'f32'),bprev,be,3,2,.01) end)
 reject(function() bound:load(bp,bprev,b{1,2,1,0,0,1,2,4,1,0,0,1},3,2,.01) end)
 reject(function() bound:bind(bp,bprev,b{1,2,1,0,0,1,2,4,1,0,0,1},3,2,.01) end)
 reject(function() bound:copy(bp,old,lambda) end)
 reject(function() bound:copy(bprev,bp,lambda) end)
 reject(function() bound:copy(out,old,bp) end)
 reject(function() bound:integrate(1,3,bp,before,gain0,gain1,after,'f64',nil,0) end)
 reject(function() bound:integrate(1,3,mobility,bp,gain0,gain1,after,'f64',nil,0) end)
 -- A valid descriptor stored in state is still forbidden as a phase argument.
 local packed=b{1,1,2,0,1,0,0,0,0};local prv=v.buffer(9)
 local alias_ws=v.constraints(3,0);alias_ws:bind(packed,prv,b{},3,0,.01)
 assert(not pcall(function() alias_ws:solve(1,packed,1) end))
 assert(not pcall(function() alias_ws:integrate(1,3,mobility,before,gain0,gain1,after,'f64',packed,1) end))
 assert(not pcall(function() bound:damp(bp,0,0,1,1e-8) end))
 bound:node(3,1e6,0,0,-1e6,0,0)
 reject(function() bound:integrate(1,3,mobility,before,gain0,gain1,after,'f64',nil,0) end)
 -- Same-buffer rebind resets results/parity/span and retains inactive tails.
 bp:copy(p,1,1,9);bprev:copy(prev,1,1,9)
 bound:bind(bp,bprev,be,3,2,.01)
 assert(bound:multipliers(1)==0 and select(2,bound:multipliers())==0)
 be:set(3,99) -- Edge metadata was copied, not bound.
 w2:load(p,prev,edges,3,2,.01)
 bound:solve(2,nil,0);bound:solve(1,nil,0);w2:solve(3,nil,0)
 w2:copy(out,old,lambda)
 for i=1,9 do assert(bp:get(i)==out:get(i) and bprev:get(i)==old:get(i)) end
 assert(bp:get(10)==91 and bprev:get(12)==92)
 -- A successful copy-load safely detaches even when its sources are bound.
 bound:load(bp,bprev,edges,3,2,.01)
 local x=bound:node(2);bp:set(4,x+1);assert(bound:node(2)==x)
 competitor:bind(bp,bprev,edges,3,2,.01)
 competitor:load(bp,bprev,edges,3,2,.01)
 -- Reduced active extent/new topology rebinds; prior buffers become free.
 local rp,rprev=b{0,0,0,1,0,0,81,82,83},b{0,0,0,1,0,0,84,85,86}
 bound:bind(rp,rprev,b{1,2,1,0,0,1},2,1,.01)
 competitor:bind(bp,bprev,edges,3,2,.01)
 bound:solve(2,nil,0);assert(rp:get(7)==81 and rprev:get(9)==86)
 competitor:load(bp,bprev,edges,3,2,.01)
 bound:bind(bp,bprev,edges,3,2,.01)
 -- Strong buffer references / weak workspace ownership, in both GC modes.
 for _,mode in ipairs{'incremental','generational'} do
  collectgarbage(mode)
  local weak=setmetatable({}, {__mode='v'})
  local owner=v.constraints(2,1)
  do
   local a,c=b{0,0,0,2,0,0},b{0,0,0,1,0,0}
   weak[1],weak[2],weak[3]=a,c,owner;owner:bind(a,c,b{1,2,1,0,0,1},2,1,.01)
  end
  collectgarbage('collect');assert(weak[1] and weak[2]);owner:solve(1,nil,0)
  local a,c=weak[1],weak[2];owner=nil
  collectgarbage('collect');collectgarbage('collect');assert(weak[3]==nil)
  local replacement=v.constraints(2,1);replacement:bind(a,c,b{1,2,1,0,0,1},2,1,.01)
 end
 collectgarbage('incremental')
 local single=v.constraints(1,0);single:bind(b{1,2,3},b{0,1,2},b{},1,0,.01)
 assert(single:multipliers()==nil and select(2,single:multipliers())==0)
 assert(not pcall(function() single:multipliers(1) end))
 -- Maximum bound extents and all ordered edges remain supported.
 local max=v.constraints(256,512);local a,c,e=v.buffer(768),v.buffer(768),v.buffer(3072)
 for i=1,512 do local j=(i-1)%255+1;e:set(6*i-5,j);e:set(6*i-4,j+1);e:set(6*i-3,1) end
 max:bind(a,c,e,256,512,.01);max:solve(32,nil,0);assert(max:multipliers(512)==0)
end
-- Displacement reads current authoritative state, subtracting before rounding.
local displacement=v.buffer(12,'f32')
local float_inputs={}
for i,source in ipairs({before,gain0,gain1}) do
  local x=v.buffer(9,'f32');for j=1,9 do x:set(j,source:get(j)) end;float_inputs[i]=x
end
do
  local a,c=b{999999.001,-.0000000001,1, 2,3,4},b{999999,0,1, 1,1,1}
  local q=v.constraints(2,0);local e=b{}
  for _,method in ipairs({'load','bind'}) do
    q[method](q,a,c,e,2,0,.01);displacement:fill(73)
    q:displacements(displacement,1,2)
    near(displacement:get(1),.001,1e-9);assert(displacement:get(2)<0)
    assert(displacement:get(4)==1 and displacement:get(5)==2 and displacement:get(6)==3)
    assert(displacement:get(7)==73)
    q:node(2,5,6,7,4,4,4);q:displacements(displacement,2,1)
    assert(displacement:get(1)==1 and displacement:get(2)==2 and displacement:get(3)==3)
    q:displacements(displacement,2,0);assert(displacement:get(1)==1)
    for _,args in ipairs({{0,1},{3,0},{1,3},{1.5,1},{1,-1}}) do
      assert(not pcall(q.displacements,q,displacement,args[1],args[2]))
    end
    assert(not pcall(q.displacements,q,v.buffer(6),1,2))
    assert(not pcall(q.displacements,q,v.buffer(2,'f32'),1,1))
    q:node(2,1e6,0,0,-1e6,0,0);displacement:fill(73)
    assert(not pcall(q.displacements,q,displacement,1,2))
    for i=1,12 do assert(displacement:get(i)==73) end
    assert(q:node(2)==1e6 and q:multipliers()==nil)
    a:load{999999.001,-.0000000001,1,2,3,4};c:load{999999,0,1,1,1,1}
  end
  assert(not pcall(v.constraints(1,0).displacements,v.constraints(1,0),displacement,1,0))
end
do
 local max=v.constraints(256,0);local a,c=v.buffer(768),v.buffer(768)
 a:fill(1);c:fill(.5);max:bind(a,c,b{},256,0,.01)
 local output=v.buffer(771,'f32');output:fill(17)
 max:displacements(output,1,256)
 assert(output:get(768)==.5 and output:get(769)==17)
 a:set(768,.25);max:displacements(output,256,1);assert(output:get(3)==-.25)
end
-- Every coefficient channel independently selects f32/f64; all are rounded
-- at the same source arithmetic boundary. Existing all-f64 float mode is oracle.
for mask=0,7 do
  local inputs={before,gain0,gain1}
  for i=1,3 do if mask & (1 << (i-1)) ~= 0 then inputs[i]=float_inputs[i] end end
  reset();w:integrate(1,3,mobility,before,gain0,gain1,after,'displacement-f32',bounds,1)
  snapshot();local expected={};for i=1,9 do expected[i]=out:get(i) end
  reset();w:integrate(1,3,mobility,inputs[1],inputs[2],inputs[3],after,'displacement-f32',bounds,1)
  snapshot();for i=1,9 do assert(out:get(i)==expected[i]) end
  if mask>0 then fail(function() w:integrate(1,3,mobility,inputs[1],inputs[2],inputs[3],after,'f64',nil,0) end) end
end
fail(function() w:integrate(1,3,v.buffer(3,'f32'),before,gain0,gain1,after,'displacement-f32',nil,0) end)
fail(function() w:integrate(1,3,mobility,before,gain0,gain1,v.buffer(9,'f32'),'displacement-f32',nil,0) end)
fail(function() w:integrate(1,3,mobility,before,gain0,gain1,after,'displacement-f32',v.buffer(5,'f32'),1) end)
fail(function() w:integrate(1,3,mobility,float_inputs[1],float_inputs[1],gain1,after,'displacement-f32',nil,0) end)
fail(function() w:integrate(1,3,mobility,v.buffer(8,'f32'),gain0,gain1,after,'displacement-f32',nil,0) end)
do
 local huge=v.buffer(9,'f32');huge:fill(1e6)
 fail(function() w:integrate(1,3,mobility,float_inputs[1],huge,huge,after,'displacement-f32',nil,0) end)
end
return function()
  reset()
  w:begin(.01)
  w:displacements(displacement,1,3)
  w:integrate(1,3,mobility,before,gain0,gain1,after,'displacement-f32',bounds,1)
  w:node(1,0,0,0,0,0,0)
  w:edge(1,1,2,1,.0001,0,2)
  w:span(1,3,1.8,.0001,0,1)
  w:solve(6,bounds,1)
  w:damp(mobility,.25,.4,.998,1e-8)
  w:copy(out,old,lambda)
  -- Warm binding execution has no bind/allocation or full-state export.
  bp:copy(p,1,1,9);bprev:copy(prev,1,1,9)
  bound:begin(.01)
  bound:displacements(displacement,1,3)
  bound:integrate(1,3,mobility,float_inputs[1],float_inputs[2],float_inputs[3],after,'displacement-f32',bounds,1)
  bound:span(1,3,1.8,.0001,0,1)
  bound:solve(6,bounds,1);bound:damp(mobility,.25,.4,1,1e-8)
  bound:multipliers(1)
  v.scatter(bp,bp,2,2,4)
end
