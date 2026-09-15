-- No argument retains every original f64 input and tolerance.
local kind = ...
local tolerance = kind == 'f32' and 1e-4 or 1e-9
local tiny = kind == 'f32' and 1e-40 or 1e-320
local numeric, g = require('vmath'), require('geometry')
local v = setmetatable({buffer = function(n) return numeric.buffer(n, kind) end},
                       {__index = numeric})
local function b(t, capacity)
  local out = v.buffer(capacity or #t); out:load(t); return out
end
local function near(a, z) assert(math.abs(a-z) < tolerance, tostring(a)..' != '..tostring(z)) end
local function bad(fn) assert(not pcall(fn)) end
local a = b({1,2,3,4,5,6})
assert(#a == 6 and #v.buffer(0) == 0)
a:copy(a,2,1,5); assert(a:get(6)==5 and a:get(2)==1)
a:fill(2); a:set(3,4); a:load({9}); assert(a:get(2)==2)
bad(function() a:load({1, 0/0}) end); assert(a:get(1)==9)
for _,x in ipairs({0,-1,7,1.5}) do bad(function() a:get(x) end) end
bad(function() a:set(1,math.huge) end)
bad(function() a:set(1,'3') end)
bad(function() v.buffer(65537) end)
bad(function() v.buffer("3") end)
bad(function() a:copy(a,4,1,4) end)
bad(function() a:load({1,2,3,4,5,6,7}) end)
near(v.lerp(2,4,1.5),5); near(v.smoothstep(0,2,1),.5)
assert(v.clamp(3,0,2)==2)
bad(function() v.clamp(0,2,1) end)
bad(function() v.smoothstep(1,1,0) end)
bad(function() v.lerp(1e6,-1e6,1e6) end)
local x,speed=v.spring(0,0,1,10,2,0,.1); near(x,.1);near(speed,1)
bad(function() v.spring(0,0,0,-1,0,0,.1) end)
bad(function() v.spring(0,0,0,1,0,0,1e-300) end)
local out, src, co = b({99,99,99}), b({1,2,3}), b({1,2,3})
v.polynomial(out,src,co,3);near(out:get(2),17)
v.combine(out,src,co,2,-1,1,3);near(out:get(3),4)
v.clamp_bulk(out,out,2,3,3);assert(out:get(1)==2 and out:get(3)==3)
near(v.dot(src,co,3),14)
local interleaved=b({1,10,2,20,3,30})
v.gather(out,interleaved,2,2,3);assert(out:get(3)==30)
v.scatter(interleaved,co,2,2,3);assert(interleaved:get(6)==3 and interleaved:get(5)==3)
bad(function() v.gather(out,interleaved,2,3,3) end)
-- Scattering overlapping sources must use the original packed values and
-- preserve every unwritten element, even with a large inactive capacity.
do
 local wide=v.buffer(768); local source=b{11,22,33,44,55}
 wide:fill(97);v.scatter(wide,source,2,3,5)
 for i=1,768 do
  local k=(i-2)/3+1
  assert(wide:get(i)==((k>=1 and k<=5 and k==math.floor(k)) and source:get(k) or 97))
 end
 local alias=b{1,2,3,4,5,6,7,8,9,10}
 v.scatter(alias,alias,2,2,5)
 for i,x in ipairs{1,1,3,2,5,3,7,4,9,5} do assert(alias:get(i)==x) end
 v.scatter(alias,alias,1,1,10)
 local saved={};for i=1,10 do saved[i]=alias:get(i) end
 v.scatter(alias,alias,10,1,0)
 for _,args in ipairs{{2,3,4},{1,0,1},{0,1,0},{11,1,0},{1,1,11}} do
  bad(function() v.scatter(alias,alias,table.unpack(args)) end)
  for i=1,10 do assert(alias:get(i)==saved[i]) end
 end
 local other=numeric.buffer(10,kind=='f32' and 'f64' or 'f32')
 bad(function() v.scatter(alias,other,1,1,0) end)
 for i=1,10 do assert(alias:get(i)==saved[i]) end
end
v.clamp_bulk(out,co,2,3,3)
v.combine(src,src,src,1,1,0,3);assert(src:get(3)==6)
bad(function() v.combine(out,src,co,1e6,0,0,3) end);assert(out:get(1)==2)

local vector=b({3,4,0,0,0,0})
local lengths=v.buffer(2)
v.length3(lengths,vector,2);near(lengths:get(1),5)
v.normalize3(vector,vector,2);near(vector:get(1),.6);assert(vector:get(4)==0)
vector:load({tiny,tiny,0});v.normalize3(vector,vector,1);near(vector:get(1),math.sqrt(.5))
v.multiply(lengths,lengths,lengths,2);near(lengths:get(1),25)
bad(function() v.divide(lengths,lengths,lengths,2) end);near(lengths:get(1),25)
lengths:set(2,1);v.divide(lengths,lengths,lengths,2);near(lengths:get(1),1)

local p,prev,acc,w=b({0,0,0, 2,0,0}),b({0,0,0, 1,0,0}),b({0,-10,0,0,-10,0}),b({0,1})
v.verlet(p,prev,acc,w,.1,0,2);near(p:get(4),3);near(p:get(5),-.1);assert(p:get(2)==0 and prev:get(4)==2)
bad(function() v.verlet(p,p,acc,w,.1,0,2) end)
local edges,lambda=b({1,2,1,0}),b({0})
p:load({0,0,0,2,0,0});v.relax(p,w,edges,lambda,.1,4,2,1,true)
near(p:get(4),1);near(lambda:get(1),-1);assert(p:get(1)==0)
p:set(4,.5);v.relax(p,w,edges,lambda,.1,1,2,1,true);near(p:get(4),.5);near(lambda:get(1),0)
v.relax(p,w,edges,lambda,.1,1,2,1,false);near(p:get(4),1)
edges:set(4,.01);p:set(4,2);v.relax(p,w,edges,lambda,.1,1,2,1,false);near(p:get(4),1.5)
edges:set(1,0);bad(function() v.relax(p,w,edges,lambda,.1,1,2,1,false) end);near(p:get(4),1.5)
edges:set(1,1);p:set(4,0);v.relax(p,w,edges,lambda,.1,1,2,1,false);near(p:get(4),0)
w:set(2,0);p:set(4,2);v.relax(p,w,edges,lambda,.1,1,2,1,false);near(p:get(4),2)
w:set(2,1);prev:load({0,0,0,0,0,0});v.damp(p,prev,w,.5,0,2);near(prev:get(4),1)
local cp,cv,cw=b({0,0,0,0,0,0,0,0,0}),b({-1,0,0,-3,0,0,-1,0,0}),b({1,1,1})
v.damp(cp,cv,cw,1,1,3);near(cv:get(4),-1)
-- A bounded longer chain converges and keeps its pinned anchor exact.
local n=32; local chain=v.buffer(n*3); local masses=v.buffer(n);masses:fill(1);masses:set(1,0)
local constraints=v.buffer((n-1)*4);local multipliers=v.buffer(n-1)
for i=1,n do chain:set(3*i-2,(i-1)*1.01) end
for i=1,n-1 do constraints:set(4*i-3,i);constraints:set(4*i-2,i+1);constraints:set(4*i-1,1) end
v.relax(chain,masses,constraints,multipliers,.01,32,n,n-1,false)
assert(chain:get(1)==0 and chain:get(n*3-2)<(n-1)*1.01)

local xy, matrix=b({1,2,3,4},8),b({0,-1,10,1,0,20})
g.affine2(xy,xy,matrix,2);near(xy:get(1),8);near(xy:get(2),21)
local xyz, transformed=b({1,2,3,4,5,6}),v.buffer(9)
local m3=b({1,0,0,10,0,1,0,20,0,0,1,30})
g.affine3(transformed,xyz,m3,2);near(transformed:get(6),36)
local weights=b({1,0});g.displace3(transformed,xyz,weights,1,2,3,2);near(transformed:get(3),6);near(transformed:get(6),6)
g.rotate3(transformed,xyz,weights,0,0,1,math.pi/2,2);near(transformed:get(1),-2);near(transformed:get(2),1);near(transformed:get(4),4)
bad(function() g.rotate3(transformed,xyz,weights,0,0,0,0,2) end)
g.prefix3(transformed,xyz,10,20,30,2);near(transformed:get(7),15);near(transformed:get(9),39)

local camera, mask=b({10,-10,20,30,1}),v.buffer(3)
local projected=v.buffer(12)
xyz:load({2,4,2,1,2,0})
g.project_points(projected,mask,xyz,camera,2);near(projected:get(1),30);near(projected:get(2),10);assert(mask:get(2)==0 and projected:get(3)==0)
local spans=b({0,0,0,2,4,2, 1,1,0,2,2,0, 0,0,1,1,1,1})
local ids=v.buffer(3)
assert(g.project_segments(projected,ids,spans,camera,3)==2)
near(projected:get(1),30);near(projected:get(2),10);assert(ids:get(1)==1 and ids:get(2)==3)
local pieces,tags=v.buffer(36),v.buffer(12)
spans:load({-1,0,2,1,0,2, 0,0,0,0,0,1, 0,0,0,-1,0,0})
assert(g.split_segments(pieces,tags,spans,1,0,3)==4)
assert(tags:get(1)==-1 and tags:get(3)==1 and tags:get(5)==0 and tags:get(7)==-1)
near(pieces:get(4),0);near(pieces:get(7),0)
assert(g.segments(pieces,b({1,2,3,4,5,6,7,8,9}),3)==2);near(pieces:get(7),4)
bad(function() g.project_segments(projected,projected,spans,camera,1) end)
bad(function() g.split_segments(pieces,tags,spans,0,0,1) end)
bad(function() g.split_segments(v.buffer(6),tags,spans,1,0,1) end)
local before=projected:get(1);camera:set(1,1e6);spans:set(1,1e6);spans:set(3,1)
bad(function() g.project_segments(projected,ids,spans,camera,1) end);assert(projected:get(1)==before)
camera:set(5,0);bad(function() g.project_points(projected,mask,xyz,camera,2) end)

local writer, mesh=g.mesh(4,2);local topology=b({1,1,2,65535,1,3,2,31})
assert(g.update_mesh(writer,xy,topology,4,2)==mesh)
topology:set(1,3);bad(function() g.update_mesh(writer,xy,topology,4,2) end)
topology:set(1,1);topology:set(3,3);bad(function() g.update_mesh(writer,xy,topology,4,2) end)
topology:set(3,2);assert(g.update_mesh(writer,xy,topology,4,2)==mesh)
assert(g.update_mesh(writer,xy,topology,0,0)==mesh)

-- Ordered XPBD reference: unequal adjacent edge weights, retained multipliers,
-- alternating order, and an interleaved external plane constraint.
do
  local values={0,0,0, 1.8,-.2,.3, 3.6,.4,-.1, 5.1,-.3,.2}
  local pp=b(values); local ee=b({1,2,1,.0001, 2,3,.7,.0003, 3,4,1.2,.0002})
  local ww=b({0,5, 2,7, 3,1});local ll=b({-.01,-.02,0});local ref={-.01,-.02,0}
  for pass=1,8 do
    local reverse=pass%2==0
    for k=1,3 do
      local i=reverse and 4-k or k
      local ia=(ee:get(4*i-3)-1)*3;local ib=(ee:get(4*i-2)-1)*3
      local dx,dy,dz=values[ib+1]-values[ia+1],values[ib+2]-values[ia+2],values[ib+3]-values[ia+3]
      local d=math.sqrt(dx*dx+dy*dy+dz*dz);local alpha=ee:get(4*i)/(.01*.01)
      local wa,wb=ww:get(2*i-1),ww:get(2*i);local old=ref[i]
      local next=math.min(0,old+(-(d-ee:get(4*i-1))-alpha*old)/(wa+wb+alpha))
      ref[i]=next
      for j,delta in ipairs({dx,dy,dz}) do
        values[ia+j]=values[ia+j]-wa*(next-old)/d*delta
        values[ib+j]=values[ib+j]+wb*(next-old)/d*delta
      end
    end
    v.relax_sweep(pp,ee,ww,ll,.01,4,3,reverse,true,1e-8)
    for i=1,12 do near(pp:get(i),values[i]) end
    for i=1,3 do near(ll:get(i),ref[i]) end
    -- Caller inserts a constraint without clearing edge lambda.
    pp:set(5,math.max(0,pp:get(5)));values[5]=math.max(0,values[5])
  end
  -- Ordered axial damping: next edge must see the previous edge's prev update.
  local previous={0,0,0, .4,-.2,.3, 1.6,.4,-.1, 4.1,-.3,.2}
  local pr=b(previous)
  for i=1,3 do
    local ia=(ee:get(4*i-3)-1)*3;local ib=(ee:get(4*i-2)-1)*3
    local delta={};local squared,axial=0,0
    for j=1,3 do delta[j]=values[ib+j]-values[ia+j];squared=squared+delta[j]^2
      axial=axial+(values[ib+j]-previous[ib+j]-values[ia+j]+previous[ia+j])*delta[j] end
    local d=math.sqrt(squared);local wa,wb=ww:get(2*i-1),ww:get(2*i)
    if d>=ee:get(4*i-1)*.998 and d>1e-8 and axial>0 then
      local impulse=axial*.6/(wa+wb)/squared
      for j=1,3 do previous[ia+j]=previous[ia+j]-wa*impulse*delta[j];previous[ib+j]=previous[ib+j]+wb*impulse*delta[j] end
    end
  end
  v.damp_edges(pp,pr,ee,ww,.6,.998,1e-8,4,3)
  for i=1,12 do near(pr:get(i),previous[i]) end
  -- Late invalid edge preserves both outputs despite earlier scratch updates.
  local savedp,savedl,savedprev=pp:get(4),ll:get(1),pr:get(4)
  ee:set(10,5)
  bad(function() v.relax_sweep(pp,ee,ww,ll,.01,4,3,false,true) end)
  bad(function() v.damp_edges(pp,pr,ee,ww,.6,.998,1e-8,4,3) end)
  assert(pp:get(4)==savedp and ll:get(1)==savedl and pr:get(4)==savedprev)
  ee:set(10,4)
  bad(function() v.relax_sweep(pp,ee,ww,pp,.01,4,3,false,true) end)
  bad(function() v.relax_sweep(pp,ee,ww,ll,.01,257,3,false,true) end)
  bad(function() v.relax_sweep(pp,ee,ww,ll,.01,4,513,false,true) end)
  bad(function() v.relax_sweep(pp,ee,ww,ll,.01,4,3,0,true) end)
  bad(function() v.damp_edges(pp,pp,ee,ww,.6,.998,1e-8,4,3) end)
  bad(function() v.damp_edges(pp,pr,ee,ww,1.1,.998,1e-8,4,3) end)
  ww:set(6,-1);bad(function() v.relax_sweep(pp,ee,ww,ll,.01,4,3,false,true) end)
end
do
  local pp=b({0,0,0,2,0,0});local ee=b({1,2,1,0});local ww=b({0,1});local ll=b({0})
  v.relax_sweep(pp,ee,ww,ll,.01,2,1,false,false);near(pp:get(4),1);near(ll:get(1),-1)
  pp:set(4,.5);v.relax_sweep(pp,ee,ww,ll,.01,2,1,false,false);near(pp:get(4),1)
  pp:set(4,0);v.relax_sweep(pp,ee,ww,ll,.01,2,1,false,true);near(pp:get(4),0)
  pp:set(4,2);ww:fill(0);v.relax_sweep(pp,ee,ww,ll,.01,2,1,false,true);near(pp:get(4),2)
  ww:set(2,1);local pr=b({0,0,0,3,0,0})
  v.damp_edges(pp,pr,ee,ww,1,1,0,2,1);near(pr:get(4),3) -- approaching
  pr:set(4,1);v.damp_edges(pp,pr,ee,ww,1,3,0,2,1);near(pr:get(4),1) -- slack
  v.damp_edges(pp,pr,ee,ww,1,1,2,2,1);near(pr:get(4),1) -- epsilon equality
  v.damp_edges(pp,pr,ee,ww,1,2,0,2,1);near(pr:get(4),2) -- threshold equality
end
do
  local x=b({-1.5,0,1.5});local d=b({9,9,9});local ids=b({3,1,2})
  v.map(d,x,'floor',3);near(d:get(1),-2);near(d:get(3),1)
  v.map(d,x,'abs',3);near(d:get(1),1.5)
  v.map(d,x,'sin',3);near(d:get(1),math.sin(-1.5))
  v.map(d,x,'cos',3);near(d:get(3),math.cos(1.5))
  v.map(d,d,'sqrt',3);near(d:get(3),math.sqrt(math.cos(1.5)))
  local saved=d:get(1);bad(function() v.map(d,x,'sqrt',3) end);assert(d:get(1)==saved)
  bad(function() v.map(d,x,'unknown',3) end)
  v.select_le(d,x,0,x,ids,3);near(d:get(1),-1.5);near(d:get(2),0);near(d:get(3),2)
  v.take(x,x,ids,1,3);near(x:get(1),1.5);near(x:get(2),-1.5)
  local rows=b({1,2,3,4,5,6});v.take(rows,rows,b({3,1}),2,2)
  near(rows:get(1),5);near(rows:get(4),2);near(rows:get(5),5)
  ids:set(3,4);saved=d:get(1);bad(function() v.take(d,x,ids,1,3) end);assert(d:get(1)==saved)
  bad(function() v.take(d,x,ids,0,3) end)
  bad(function() v.select_le(d,x,0,x,ids,4) end)
end

-- Overflow and empty-prefix calls preserve all published outputs.
do
  local pp,ee,ww,ll=b({0,0,0,1,0,0})
  ee=b({1,2,1e6,0});ww=b({0,1e-6});ll=b({7})
  bad(function() v.relax_sweep(pp,ee,ww,ll,.01,2,1,false,false) end)
  assert(pp:get(4)==1 and ll:get(1)==7)
  v.relax_sweep(pp,ee,ww,ll,.01,2,0,false,false)
  assert(pp:get(4)==1 and ll:get(1)==7)
  pp:load({-1e6,0,0,1e6,0,0});local pr=b({1e6,0,0,-1e6,0,0})
  ww:set(2,1)
  bad(function() v.damp_edges(pp,pr,ee,ww,1,0,0,2,1) end)
  assert(pr:get(1)==1e6 and pr:get(4)==-1e6)
  local empty=v.buffer(0)
  v.map(empty,empty,'floor',0);v.select_le(empty,empty,0,empty,empty,0)
  v.take(empty,empty,empty,1,0)
end

-- Consumer formula fixtures: hot arithmetic remains bulk, references are scalar.
do
  local n=8
  local x,y,z,ux,uy,uz={}, {}, {}, {}, {}, {}
  for i=1,n do x[i]=i*.1;y[i]=(i-4)*.02;z[i]=2+i*.1
    ux[i]=i*.002;uy[i]=-i*.003;uz[i]=i*.001 end
  local X,Y,Z,U,V,W=b(x),b(y),b(z),b(ux),b(uy),b(uz)
  local travel=v.buffer(n);local packed=v.buffer(3*n)
  v.scatter(packed,U,1,3,n);v.scatter(packed,V,2,3,n);v.scatter(packed,W,3,3,n)
  v.length3(travel,packed,n)
  local one=v.buffer(n);one:fill(1)
  local factor,tmp=v.buffer(n),v.buffer(n)
  local result={v.buffer(n),v.buffer(n),v.buffer(n)}
  local h=.01
  -- Both interior drag regimes, selected by the original y channel.
  for axis,old in ipairs({X,Y,Z}) do
    local displacement=({U,V,W})[axis]
    v.combine(tmp,travel,travel,.3,0,1+5*h,n);v.divide(factor,one,tmp,n)
    local wet=v.buffer(n);v.multiply(wet,displacement,factor,n)
    v.combine(wet,old,wet,1,1,axis==2 and .15*h*h or 0,n)
    if axis==2 then v.clamp_bulk(wet,wet,-1e6,0,n) end
    v.combine(tmp,travel,travel,.016,0,1,n);v.divide(factor,one,tmp,n)
    local air=v.buffer(n);v.multiply(air,displacement,factor,n)
    if axis~=2 then
      v.combine(tmp,air,air,1/(1+10*h),0,0,n);v.select_le(air,Y,.005,tmp,air,n)
    end
    v.combine(air,old,air,1,1,axis==2 and -9.81*h*h or 0,n)
    v.select_le(result[axis],Y,0,wet,air,n)
    for i=1,n do
      local tr=math.sqrt(ux[i]^2+uy[i]^2+uz[i]^2)
      local u=({ux,uy,uz})[axis][i];local expected
      if y[i]<=0 then
        expected=({x,y,z})[axis][i]+u/(1+5*h+.3*tr)+(axis==2 and .15*h*h or 0)
        if axis==2 then expected=math.min(0,expected) end
      else
        u=u/(1+.016*tr);if axis~=2 and y[i]<=.005 then u=u/(1+10*h) end
        expected=({x,y,z})[axis][i]+u+(axis==2 and -9.81*h*h or 0)
      end
      near(result[axis]:get(i),expected)
    end
  end
  -- Per-segment transverse force followed by length preservation.
  local len,axial,gain=v.buffer(n),v.buffer(n),v.buffer(n)
  v.length3(len,packed,n)
  v.combine(axial,U,V,.2,-.3,0,n);v.combine(axial,axial,W,1,.4,0,n)
  v.clamp_bulk(tmp,len,1e-8,1e6,n);v.divide(axial,axial,tmp,n)
  gain:fill(.7);v.multiply(axial,axial,gain,n);v.combine(axial,axial,axial,-1,0,1,n)
  v.multiply(gain,gain,len,n)
  local bent=v.buffer(3*n)
  for axis,component in ipairs({U,V,W}) do
    v.multiply(tmp,component,axial,n);v.combine(tmp,tmp,gain,1,({.2,-.3,.4})[axis],0,n)
    v.scatter(bent,tmp,axis,3,n)
  end
  v.length3(tmp,bent,n);v.divide(factor,len,tmp,n)
  for axis=1,3 do v.gather(tmp,bent,axis,3,n);v.multiply(tmp,tmp,factor,n);v.scatter(bent,tmp,axis,3,n) end
  for i=1,n do
    local ll=math.sqrt(ux[i]^2+uy[i]^2+uz[i]^2)
    local dot=(.2*ux[i]-.3*uy[i]+.4*uz[i])/ll
    local nx=ux[i]*(1-.7*dot)+.2*ll*.7
    local ny=uy[i]*(1-.7*dot)-.3*ll*.7
    local nz=uz[i]*(1-.7*dot)+.4*ll*.7
    local norm=math.sqrt(nx*nx+ny*ny+nz*nz)
    for axis,value in ipairs({nx,ny,nz}) do near(bent:get(3*i-3+axis),value*ll/norm) end
  end
  -- Authored deformation -> planar transform -> perspective -> grid -> scale.
  local flex,sway,scale,angle=.7,.2,1.3,.4
  v.combine(tmp,X,X,1/30,0,0,n);v.multiply(tmp,tmp,tmp,n)
  v.combine(tmp,Y,tmp,1,flex,0,n);v.combine(tmp,tmp,Z,1,sway,0,n)
  local xy=v.buffer(2*n);v.scatter(xy,X,1,2,n);v.scatter(xy,tmp,2,2,n)
  local ca,sa=math.cos(angle),math.sin(angle)
  g.affine2(xy,xy,b({scale*ca,-scale*sa,10,scale*sa,scale*ca,20}),n)
  local uu,ww,depth,perspective=v.buffer(n),v.buffer(n),v.buffer(n),v.buffer(n)
  v.gather(uu,xy,1,2,n);v.gather(ww,xy,2,2,n)
  v.combine(depth,uu,ww,-.5,.866,0,n);v.combine(tmp,depth,depth,1/1300,0,1,n)
  v.divide(perspective,one,tmp,n)
  v.combine(tmp,uu,ww,.866,.5,0,n);v.multiply(tmp,tmp,perspective,n)
  v.combine(tmp,tmp,tmp,1,0,174+2,n);v.scatter(xy,tmp,1,2,n)
  v.multiply(tmp,depth,perspective,n);v.combine(tmp,tmp,perspective,-.67,-3,259+4,n)
  v.scatter(xy,tmp,2,2,n)
  for i=1,n do
    local bb=y[i]+z[i]*sway+flex*(x[i]/30)^2
    local u=10+(x[i]*ca-bb*sa)*scale;local w=20+(x[i]*sa+bb*ca)*scale
    local dd=-u*.5+w*.866;local pp=1/(1+dd/1300)
    near(xy:get(2*i-1),174+(u*.866+w*.5)*pp+2)
    near(xy:get(2*i),259-dd*.67*pp-3*pp+4)
  end
  local unrounded=v.buffer(2*n);unrounded:copy(xy,1,1,2*n)
  v.combine(xy,xy,xy,.5,0,.5,2*n);v.map(xy,xy,'floor',2*n)
  v.combine(xy,xy,xy,2,0,0,2*n);v.combine(xy,xy,xy,.8,0,0,2*n)
  for i=1,2*n do near(xy:get(i),math.floor(unrounded:get(i)/2+.5)*2*.8) end
  -- Reverse source rows without reversing endpoints; preserve IDs through clipping.
  local points,rows=b({0,-1,0,1,0,2,2,1,3}),v.buffer(12)
  g.segments(rows,points,3);v.take(rows,rows,b({2,1}),6,2)
  near(rows:get(1),1);near(rows:get(4),2)
  local pieces,tags=v.buffer(24),v.buffer(8)
  assert(g.split_segments(pieces,tags,rows,2,0,2)==2)
  -- Negative-to-zero is tagged negative; consumer must use both-endpoints<0.
  assert(tags:get(3)==-1 and rows:get(8)<0 and rows:get(11)==0)
  local screen,ids=v.buffer(8),v.buffer(2)
  g.affine3(pieces,pieces,b({1,0,0,0,0,-1,0,1.6,0,0,1,0}),4)
  assert(g.project_segments(screen,ids,pieces,b({260,260,184,203,.25}),2)==2)
  near(screen:get(1),184+260/2);near(screen:get(2),203+260*1.6/2)
end

-- Refined norm is opt-in f64; legacy tiny length/normalize retain scaling.
if kind ~= 'f32' then
  local src=b{3,4,0, 1e-320,0,0, 0,0,0}
  local out=b{91,92,93,94}
  v.length3_refined(out,src,3)
  assert(out:get(1)==5 and out:get(2)==0 and out:get(3)==0 and out:get(4)==94)
  v.length3(out,src,3);assert(out:get(2)>0)
  v.normalize3(out,src,1);near(out:get(1),.6)
  src:load{3,4,0, 5,12,0};v.length3_refined(src,src,2)
  assert(src:get(1)==5 and src:get(2)==13 and src:get(3)==0 and src:get(4)==5)
  src:load{3,4,0, 1e6,1e6,1e6};out:fill(71)
  bad(function() v.length3_refined(out,src,2) end)
  for i=1,#out do assert(out:get(i)==71) end
  for _,n in ipairs({-1,.5,21846,'1'}) do bad(function() v.length3_refined(out,src,n) end) end
  bad(function() v.length3_refined(v.buffer(0),src,1) end)
  bad(function() v.length3_refined(out,v.buffer(2),1) end)
  v.length3_refined(out,src,0);assert(out:get(1)==71)
  bad(function() v.length3_refined(out,numeric.buffer(3,'f32'),0) end)
  bad(function() v.length3_refined(numeric.buffer(3,'f32'),src,0) end)
  local maxsrc=v.buffer(65535);local maxout=v.buffer(21845)
  v.length3_refined(maxout,maxsrc,21845);assert(maxout:get(21845)==0)
else
  bad(function() v.length3_refined(v.buffer(3),v.buffer(3),1) end)
end

-- The C harness turns on allocator counting after compiling/initializing this
-- closure. Exercise every successful hot API repeatedly, including mesh writes.
local qa,qb,qc=b({1,2,3,4,5,6,7,8,9,10,11,12}),v.buffer(12),b({1})
local qm=b({1,0,0,0,1,0,0,0,1,0,0,0})
local qcam,qmask=b({1,1,0,0,.1}),v.buffer(4)
local qs,qt,qo=b({0,0,1,1,1,1}),v.buffer(4),v.buffer(12)
local qp,qprev,qacc,qw=b({0,0,0,1,0,0}),b({0,0,0,1,0,0}),v.buffer(6),b({0,1})
local qe,ql=b({1,2,1,0}),v.buffer(1)
local qwgt=b({0,0,0,0})
local qids,qedgeweights=b({2,1}),b({0,1})
local load_values = {1,2,3,4,5,6,7,8,9,10,11,12}
return function()
  for _=1,100 do
    qa:load(load_values);assert(#qa==12)
    qa:set(1,1);qa:get(1);qb:fill(0);qb:copy(qa,1,1,12)
    v.clamp(1,0,2);v.lerp(0,1,.5);v.smoothstep(0,1,.5);v.spring(0,0,1,1,1,0,.01)
    v.multiply(qb,qa,qa,12);v.divide(qb,qa,qa,12);v.length3(qb,qa,4);v.normalize3(qb,qa,4)
    if kind ~= 'f32' then v.length3_refined(qb,qa,4) end
    v.gather(qb,qa,1,2,6);v.scatter(qb,qa,1,2,6)
    v.combine(qb,qa,qa,1,0,0,12);v.polynomial(qb,qa,qc,12);v.clamp_bulk(qb,qa,0,10,12);v.dot(qc,qc,1)
    v.verlet(qp,qprev,qacc,qw,.01,1,2);v.relax(qp,qw,qe,ql,.01,2,2,1,true);v.damp(qp,qprev,qw,1,.5,2)
    g.affine2(qb,qa,qm,2);g.affine3(qb,qa,qm,2);g.displace3(qb,qa,qwgt,1,2,3,2);g.rotate3(qb,qa,qwgt,0,0,1,1,2)
    g.prefix3(qb,qa,0,0,0,2);g.project_points(qb,qmask,qa,qcam,2);g.segments(qo,qp,2)
    g.project_segments(qb,qmask,qs,qcam,1);g.split_segments(qo,qt,qs,1,.5,1)
    v.relax_sweep(qp,qe,qedgeweights,ql,.01,2,1,true,true)
    v.damp_edges(qp,qprev,qe,qedgeweights,.5,.998,1e-8,2,1)
    v.map(qb,qa,"abs",12);v.map(qb,qa,"sqrt",12);v.map(qb,qa,"sin",12);v.map(qb,qa,"cos",12)
    v.map(qb,qa,"floor",12);v.select_le(qb,qa,2,qa,qb,12);v.take(qb,qa,qids,3,2)
    g.update_mesh(writer,xy,topology,4,2)
  end
end
