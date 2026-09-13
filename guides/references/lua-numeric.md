# Lua numeric and geometry API

The Host preloads `vmath` and `geometry` on desktop, embedded and browser targets.
Standard Lua `math` is unchanged. Neither module needs an app extension or a
Display device. The production contract is `libs/lua/include/h2_lua_numeric.h`.

Allocate buffers and meshes during setup. Buffers store binary64 values and an
equal-sized private scratch area (approximately 16 bytes per scalar plus userdata
overhead), all charged to the VM memory budget. Successful hot calls allocate
nothing; error construction may allocate. Indices are one-based integers. Every
numeric input/result must be finite and within ±1e6. Invalid types, bounds,
parameters, topology or numeric overflow raise Lua errors without changing any
published output. Prefix operations leave unused suffixes unchanged. Multiple
outputs must differ; input/output aliasing is otherwise supported except for the
physics restrictions below. No resizing, views or retained C pointers exist.

| API | Result and semantics |
| --- | --- |
| `vmath.buffer(count)` | Zeroed buffer, count 0..65536. |
| `#b` | Fixed scalar count. |
| `b:get(index)` | Scalar value. |
| `b:set(index,value)` | Store one scalar; no result. |
| `b:fill(value)` | Fill all scalars; no result. |
| `b:load(flat_table)` | Raw dense prefix import; no result. |
| `dst:copy(src,dst_first,src_first,count)` | Overlap-safe range copy; no result. |
| `vmath.clamp(x,lo,hi)` | Closed-interval clamp; lo ≤ hi. |
| `vmath.lerp(a,b,t)` | `a+(b-a)*t`, including extrapolation. |
| `vmath.smoothstep(lo,hi,x)` | Clamped cubic `t*t*(3-2*t)`; lo < hi. |
| `vmath.spring(x,v,target,k,c,accel,dt)` | New x,v; semi-implicit Euler, `v+=(k*(target-x)-c*v+accel)*dt`, `x+=v*dt`; k,c ≥ 0, 1e-6 ≤ dt ≤ .1. |
| `vmath.combine(dst,a,b,ka,kb,bias,count)` | Store `ka*a[i]+kb*b[i]+bias`; no result. |
| `vmath.polynomial(dst,src,coeff,count)` | Horner evaluation; 1..9 ascending-power coefficients including constant; no result. |
| `vmath.clamp_bulk(dst,src,lo,hi,count)` | Component clamp; no result. |
| `vmath.gather(dst,src,first,stride,count)` | Pack a strided scalar channel; stride ≥ 1; no result. |
| `vmath.scatter(dst,src,first,stride,count)` | Write packed src into strided dst indices; preserve other values; no result. |
| `vmath.multiply(dst,a,b,count)` | Componentwise multiplication; no result. |
| `vmath.divide(dst,a,b,count)` | Componentwise division; zero denominator raises an error; no result. |
| `vmath.length3(dst,src,n)` | n xyz triples to n scalar lengths; n ≤ 21845; no result. |
| `vmath.normalize3(dst,src,n)` | Unit xyz vectors, zero stays zero; n ≤ 21845; no result. |
| `vmath.dot(a,b,count)` | Scalar dot product of prefixes. |
| `vmath.verlet(p,prev,accel,inv_mass,dt,drag,n)` | Packed xyz; `p+=(p-prev)/(1+drag*dt)+accel*dt²`, prev=old p; no result. |
| `vmath.relax(p,inv_mass,edges,lambda,dt,iterations,n,m,tension_only)` | XPBD distance relaxation, writes p and lambda; no result. |
| `vmath.damp(p,prev,inv_mass,retain,blend,n)` | Replaces displacement with `retain*lerp(displacement,neighbor_mean,blend)`, writes prev; no result. |
| `geometry.affine2(dst,src,matrix,n)` | Packed xy, row-major 2×3 affine matrix; no result. |
| `geometry.affine3(dst,src,matrix,n)` | Packed xyz, row-major 3×4 affine matrix; no result. |
| `geometry.displace3(dst,src,weights,dx,dy,dz,n)` | Add weight times displacement to each xyz; no result. |
| `geometry.rotate3(dst,src,weights,ax,ay,az,angle,n)` | Rodrigues rotation around origin, normalized axis, radians angle×weight; axis length ≥ 1e-12; no result. |
| `geometry.prefix3(dst,segments,x,y,z,n)` | n xyz displacement vectors to n+1 points starting at x,y,z; no result. |
| `geometry.project_points(dst,mask,src,camera,n)` | xyz to xy; hidden points write (0,0), mask 0; visible mask 1; no result. |
| `geometry.segments(dst,points,n)` | n xyz polyline points to n−1 independent six-scalar segment rows; returns n−1. |
| `geometry.project_segments(dst,ids,src,camera,n)` | Clip six-scalar rows, compact visible four-scalar xy endpoint rows and original segment IDs; returns visible count. |
| `geometry.split_segments(dst,tags,src,axis,offset,n)` | Split at axis 1/2/3 coordinate=offset; six-scalar output rows, two-scalar `{side,source_index}` tags; returns piece count. |
| `geometry.mesh(vertex_capacity,primitive_capacity)` | Returns writer,public_display_mesh, initially empty; limits 32768 vertices/4096 primitives. |
| `geometry.update_mesh(writer,xy,topology,nv,np)` | Returns same mesh; topology rows `{kind,first,count,rgb565}`, 0 polygon (3..128 vertices), 1 line (2); RGB565 integer 0..65535. No drawing/presenting. |

Physics uses 1e-6 ≤ dt ≤ .1 and n ≤ 256. Inverse masses must be nonnegative; zero
pins nodes. Verlet takes already mass-scaled accelerations, uses positive mass as
a mobility flag, and leaves both states of pinned nodes unchanged. Its writable
p/prev must differ from each other and from accel/inv_mass. Drag is nonnegative.
Damping requires retain/blend in [0,1], leaves pinned prev unchanged, and uses each
endpoint's own displacement as its neighbor mean; prev must differ from p/mass.

Relaxation accepts n ≥ 1, m ≤ 512, iterations 1..32. Edge rows are
`{node_a,node_b,rest_length,compliance}` with distinct valid indices and
nonnegative rest/compliance. Lambda resets to zero each call; sweeps alternate
forward/backward. `tension_only=true` clamps lambda ≤ 0, false is bilateral.
Coincident endpoints (distance < 1e-12) and fully pinned edges are skipped.
`lambda/dt²` represents constraint force. Writable p/lambda must differ from each
other and all input buffers. There is no implicit collision or material policy.

Camera layout is `{fx,fy,cx,cy,near}` with near ≥ .001, +Z forward and projection
`(cx+fx*x/z,cy+fy*y/z)`. Use affine3 for camera positioning and negative fy for
screen Y inversion. On-plane points are visible. Segments wholly behind near are
discarded. Projection preserves source order/direction. Split side is −1/+1 for
negative/positive, 0 only for wholly coplanar segments; touching endpoints do not
create extra pieces. Tags and IDs let Lua choose shading independently.

All transforms and point projection accept n ≤ 21845; prefix3 n ≤ 21844;
polyline expansion accepts 1..4097 points; segment projection n ≤ 4096; splitting
n ≤ 2048. Buffers must fit every requested scalar prefix. Segment projection
requires worst-case space for n output rows/IDs; splitting requires 2*n output
rows/tags even if no segment crosses. Mesh active counts must fit its capacities.

```lua
local v, g = require('vmath'), require('geometry')
local xyz, xy, mask, camera = v.buffer(6), v.buffer(4), v.buffer(2), v.buffer(5)
xyz:load({0,0,2, 1,1,2})
camera:load({100,-100,160,120,.1})
local topology = v.buffer(4)
topology:load({1,1,2,65535})
local writer, mesh = g.mesh(2,1)
-- Reuse this storage in the frame loop:
g.project_points(xy,mask,xyz,camera,2)
g.update_mesh(writer,xy,topology,2,1)
-- Submit mesh using the existing Display batch API.
```

Binary64 retains small displacements; bulk calls avoid per-element Lua/C calls.
There is no guarantee of cross-platform bit identity or an ESP32-S3 frame rate;
measure real target workloads. Game formulas, camera constants, material choices,
colors and time-step policy belong to Lua consumers.
