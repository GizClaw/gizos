local v, g = require('vmath'), require('geometry')
local function b(values, kind)
  local out = v.buffer(#values, kind or 'f32'); out:load(values); return out
end
local function bad(fn, message)
  local ok, err = pcall(fn)
  assert(not ok)
  if message then assert(string.find(err, message, 1, true), err) end
end
local a = b({.1, 1e6, -1e6})
assert(a:get(1) ~= .1 and math.abs(a:get(1) - .1) < 1e-8)
assert(a:get(2) == 1e6 and a:get(3) == -1e6)
assert(b({.1}, 'f64'):get(1) == .1 and b({.1}, nil):get(1) ~= .1)
for _, kind in ipairs({'float', '', 32, false, {}}) do
  bad(function() v.buffer(3, kind) end)
end
for _, x in ipairs({1e6 + .001, -1e6 - .001, math.huge, -math.huge, 0/0}) do
  bad(function() a:set(1, x) end)
  bad(function() a:fill(x) end)
  bad(function() a:load({1, x}) end)
  assert(a:get(1) ~= 1 and a:get(2) == 1e6)
end
-- Arithmetic must round in float, not just on the final store: binary32 loses
-- the .01 term at 1e6 while binary64 retains it through cancellation.
local x, y, out = b({1e6}), b({.01}), b({9})
v.combine(out, x, y, 1, 1, -1e6, 1); assert(out:get(1) == 0)
local dx, dy, dout = b({1e6}, 'f64'), b({.01}, 'f64'), b({9}, 'f64')
v.combine(dout, dx, dy, 1, 1, -1e6, 1); assert(dout:get(1) > .009)
assert(v.dot(b({1e6,.01,-1e6}), b({1,1,1}), 3) == 0)
a:set(1, 1e-50); assert(a:get(1) == 0)
a:load({1e-40,1e-40,0});v.normalize3(a,a,1)
assert(math.abs(a:get(1) - math.sqrt(.5)) < 1e-6)
-- Boundary parameters use the selected precision, including the minimum dt.
local p, prev, acc, mass = b({0,0,0}), b({0,0,0}), b({1,0,0}), b({1})
v.verlet(p,prev,acc,mass,1e-6,0,1)
assert(p:get(1)>0)
bad(function() v.verlet(p,prev,acc,mass,1e-7,0,1) end)
local cam = b({1,1,0,0,.001})
g.project_points(b({0,0}),b({0}),b({0,0,.001}),cam,1)

-- Check each buffer argument of every multi-buffer entry, in both directions.
-- Mismatches fail before any public change, including otherwise empty calls.
local calls = {
  {function(...) return (...):copy(select(2,...)) end, {1,2,1,1,0}, {1,2}},
  {v.combine, {1,2,3,1,1,0,0}, {1,2,3}},
  {v.polynomial, {1,2,3,0}, {1,2,3}},
  {v.clamp_bulk, {1,2,0,1,0}, {1,2}},
  {v.gather, {1,2,1,1,0}, {1,2}},
  {v.scatter, {1,2,1,1,0}, {1,2}},
  {v.multiply, {1,2,3,0}, {1,2,3}},
  {v.divide, {1,2,3,0}, {1,2,3}},
  {v.length3, {1,2,0}, {1,2}},
  {v.normalize3, {1,2,0}, {1,2}},
  {v.dot, {1,2,0}, {1,2}},
  {v.verlet, {1,2,3,4,.01,0,0}, {1,2,3,4}},
  {v.relax, {1,2,3,4,.01,1,1,0,false}, {1,2,3,4}},
  {v.relax_sweep, {1,2,3,4,.01,1,0,false,false}, {1,2,3,4}},
  {v.damp, {1,2,3,1,0,0}, {1,2,3}},
  {v.damp_edges, {1,2,3,4,1,0,0,1,0}, {1,2,3,4}},
  {v.map, {1,2,'abs',0}, {1,2}},
  {v.select_le, {1,2,0,3,4,0}, {1,2,4,5}},
  {v.take, {1,2,3,1,0}, {1,2,3}},
  {g.affine2, {1,2,3,0}, {1,2,3}},
  {g.affine3, {1,2,3,0}, {1,2,3}},
  {g.displace3, {1,2,3,0,0,0,0}, {1,2,3}},
  {g.rotate3, {1,2,3,0,0,1,0,0}, {1,2,3}},
  {g.prefix3, {1,2,0,0,0,0}, {1,2}},
  {g.project_points, {1,2,3,4,0}, {1,2,3,4}},
  {g.project_segments, {1,2,3,4,0}, {1,2,3,4}},
  {g.segments, {1,2,1}, {1,2}},
  {g.split_segments, {1,2,3,1,0,0}, {1,2,3}},
}
local writer = g.mesh(2,1)
calls[#calls+1] = {g.update_mesh, {writer,1,2,0,0}, {2,3}}
for _, kind in ipairs({'f32','f64'}) do
  for _, call in ipairs(calls) do
    for _, changed in ipairs(call[3]) do
      local args, buffers = {}, {}
      for i,arg in ipairs(call[2]) do args[i]=arg end
      for _, index in ipairs(call[3]) do
        local other = kind == 'f32' and 'f64' or 'f32'
        local buffer = v.buffer(12, index == changed and other or kind)
        buffer:fill(1)
        args[index]=buffer;buffers[#buffers+1]=buffer
      end
      bad(function() call[1](table.unpack(args)) end, 'mixed numeric buffer kinds')
      for _, buffer in ipairs(buffers) do
        for i=1,#buffer do assert(buffer:get(i)==1) end
      end
    end
  end
end
-- Late overflow preserves both Verlet outputs and the destination suffix.
p,prev,acc,mass = b({0,0,0,1e6,0,0}),b({-1,0,0,-1e6,0,0}),b({0,0,0,0,0,0}),b({1,1})
bad(function() v.verlet(p,prev,acc,mass,.01,0,2) end)
assert(p:get(1)==0 and p:get(4)==1e6 and prev:get(1)==-1 and prev:get(4)==-1e6)
local d, mask = b({7,8,9,10}), b({3,4})
bad(function() g.project_points(d,mask,b({0,0,1,1e6,0,1}),b({1e6,1,0,0,1}),2) end)
assert(d:get(1)==7 and d:get(4)==10 and mask:get(1)==3 and mask:get(2)==4)
-- Import and overlap-safe strided operations use the float-sized scratch.
local q=b({1,2,3,4,5,6})
v.gather(q,q,2,2,3);assert(q:get(1)==2 and q:get(3)==6 and q:get(4)==4)
q:load({1,2,3,4,5,6});v.scatter(q,q,2,2,3)
assert(q:get(2)==1 and q:get(4)==2 and q:get(6)==3)
