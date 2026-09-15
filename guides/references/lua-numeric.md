# Lua numeric and geometry API

The Host preloads `vmath` and `geometry` on desktop, embedded and browser targets. Standard Lua `math` is unchanged. Neither module needs an app extension or a Display device. The production contract is `libs/lua/include/h2_lua_numeric.h`.

Allocate buffers and meshes during setup. Buffers store `"f64"` (binary64, default) or `"f32"` (binary32) values and an equal-sized private scratch area (16 or 8 bytes per scalar respectively, plus fixed metadata/userdata overhead), all charged to the VM memory budget. Successful hot calls allocate nothing; error construction may allocate. Indices are one-based integers. Every numeric input/result must be finite and within ±1e6. Invalid types, bounds, parameters, topology or numeric overflow raise Lua errors without changing any published output. Prefix operations leave unused suffixes unchanged. Multiple outputs must differ; input/output aliasing is otherwise supported except for the physics restrictions below. No resizing, views or retained C pointers exist.

Every buffer operand in one call must have the same kind, including coefficients, weights, camera, masks, indices, tags and mesh topology. Mixed kinds raise `mixed numeric buffer kinds (f32/f64)` before publishing output, even for empty prefixes. There is no implicit conversion. `geometry.update_mesh` accepts f32 xy with f32 topology; the writer and Display mesh are independent of numeric precision.

All-f32 buffer calls compute in `float`, including float math functions. Lua scalar inputs are checked for finite/±1e6 bounds before conversion to float once per call; `load` converts each imported element. Parameter intervals are checked in the selected precision. Storage and arithmetic round to that precision; very small values may underflow to zero. `get` and `dot` return ordinary Lua numbers. Scalar-only `clamp`, `lerp`, `smoothstep`, `spring` and standard Lua `math` retain double semantics.

| API | Result and semantics |
| --- | --- |
| `vmath.buffer(count[,kind])` | Zeroed buffer, count 0..65536; `"f64"` (default, also for nil) or `"f32"`. |
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
| `vmath.relax_sweep(p,edges,weights,lambda,dt,n,m,reverse,tension_only[,epsilon])` | One ordered XPBD sweep retaining lambda; per-edge `{wa,wb}`; optional distance cutoff defaults to 1e-12. |
| `vmath.damp_edges(p,prev,edges,weights,blend,threshold,epsilon,n,m)` | Forward ordered removal of separating axial displacement on taut edges; writes prev. |
| `vmath.map(dst,src,operation,count)` | Componentwise `"abs"`, `"sqrt"`, `"sin"`, `"cos"`, or `"floor"`; no result. Negative sqrt fails atomically. |
| `vmath.select_le(dst,test,threshold,yes,no,count)` | Select yes[i] when test[i] ≤ threshold, otherwise no[i]; no result. |
| `vmath.take(dst,src,indices,width,n)` | Gather n rows of width scalars using one-based row indices; permits duplicates and permutations; no result. |
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

Physics uses 1e-6 ≤ dt ≤ .1 and n ≤ 256. Inverse masses must be nonnegative; zero pins nodes. Verlet takes already mass-scaled accelerations, uses positive mass as a mobility flag, and leaves both states of pinned nodes unchanged. Its writable p/prev must differ from each other and from accel/inv_mass. Drag is nonnegative. Damping requires retain/blend in [0,1], leaves pinned prev unchanged, and uses each endpoint's own displacement as its neighbor mean; prev must differ from p/mass.

Relaxation accepts n ≥ 1, m ≤ 512, iterations 1..32. Edge rows are `{node_a,node_b,rest_length,compliance}` with distinct valid indices and nonnegative rest/compliance. Lambda resets to zero each call; sweeps alternate forward/backward. `tension_only=true` clamps lambda ≤ 0, false is bilateral. Coincident endpoints (distance < 1e-12) and fully pinned edges are skipped. `lambda/dt²` represents constraint force. Writable p/lambda must differ from each other and all input buffers. There is no implicit collision or material policy.

`relax_sweep` shares relax's edge layout, n/m/dt limits and writable-buffer alias restrictions, but weights are **two values per edge**, not per node. It copies the existing lambda prefix and processes edges in forward or reverse order exactly once. With d the current distance, alpha=compliance/dt²: `next=old+(-(d-rest)-alpha*old)/(wa+wb+alpha)`; optionally clamp next to ≤0, then apply `a-=wa*(next-old)*(b-a)/d`, `b+=wb*(next-old)*(b-a)/d`. Zero distance, distance < epsilon, and wa+wb=0 are skipped. Epsilon is nonnegative, default 1e-12. Initialize lambda once before the caller's sweep loop. External constraints can run between sweeps without losing accumulated lambda.

`damp_edges` uses the same four-scalar edge rows and two-scalar weights; compliance is validated but unused. It accepts n in 1..256, m ≤512, blend in [0,1], threshold/epsilon ≥0; prev must differ from every input. For delta=b-a, d=length(delta), and current displacements u=p-prev, if d≥rest*threshold, d>epsilon, wa+wb>0 and dot(ub-ua,delta)>0, set `impulse=dot(ub-ua,delta)*blend/(wa+wb)/d²`, then `prev_a-=wa*impulse*delta`, `prev_b+=wb*impulse*delta`. Later edges observe earlier prev changes. Neither p nor lambda changes.

`map`, `select_le` and `take` use buffer-capacity bounds (at most 65536 scalars), permit all aliases, preserve unused suffixes and allocate no scratch beyond the destination's existing private storage. `take` requires width≥1, integer indices in 1..floor(#src/width), n≤#indices and n*width≤#dst. For vector conditionals, apply select_le separately to gathered scalar channels; for min(a,b), select using test=a-b, threshold=0, yes=a, no=b.

Camera layout is `{fx,fy,cx,cy,near}` with near ≥ .001, +Z forward and projection `(cx+fx*x/z,cy+fy*y/z)`. Use affine3 for camera positioning and negative fy for screen Y inversion. On-plane points are visible. Segments wholly behind near are discarded. Projection preserves source order/direction. Split side is −1/+1 for negative/positive, 0 only for wholly coplanar segments; touching endpoints do not create extra pieces. Tags and IDs let Lua choose shading independently.

All transforms and point projection accept n ≤ 21845; prefix3 n ≤ 21844; polyline expansion accepts 1..4097 points; segment projection n ≤ 4096; splitting n ≤ 2048. Buffers must fit every requested scalar prefix. Segment projection requires worst-case space for n output rows/IDs; splitting requires 2*n output rows/tags even if no segment crosses. Mesh active counts must fit its capacities.

```lua
local v, g = require('vmath'), require('geometry')
local xyz, xy, mask, camera = v.buffer(6, "f32"), v.buffer(4, "f32"), v.buffer(2, "f32"), v.buffer(5, "f32")
xyz:load({0,0,2, 1,1,2})
camera:load({100,-100,160,120,.1})
local topology = v.buffer(4, "f32")
topology:load({1,1,2,65535})
local writer, mesh = g.mesh(2,1)
-- Reuse this storage in the frame loop:
g.project_points(xy,mask,xyz,camera,2)
g.update_mesh(writer,xy,topology,2,1)
-- Submit mesh using the existing Display batch API.
```

Binary64 retains smaller displacements; binary32 halves payload storage and uses single-precision arithmetic. Bulk calls avoid per-element Lua/C calls. There is no guarantee of cross-platform bit identity or an ESP32-S3 frame rate; measure real target workloads. Game formulas, camera constants, material choices, colors and time-step policy belong to Lua consumers.

## Prepared execution and ownership

Prepared constraints, weighted rotations and geometry poses are explicit f64-input APIs. They preserve binary64 world coordinates and selectively use the extracted float/compensated arithmetic; they do not change any existing f32/f64 operation. Their complete signatures, capacities, precision, reset and failure rules are specified in the generated [numeric API reference](./lua.md). Direct pose/polyline drawing and RGB888 gradient descriptors are specified in the [Display API reference](./lua.md).

A constraint workspace normally copies current/previous positions through `load`. Alternatively, Lua can create standard f64 buffers and explicitly `bind(p,previous,edges,n,m,dt)` their state storage. Bind keeps those two buffers alive and exclusively owned by one live workspace; it still copies edge metadata and retains private transactional scratch. Public buffer writes and `node` patches are visible to each other between synchronous calls. They do not reset multipliers or refresh material coefficients. Rebind initializes the active topology and resets lambda/parity/span; successful `load` safely copies into owned storage and releases prior bindings. Failed bind/load preserves the existing state and ownership. Weak owner records permit GC/VM teardown without keeping a dead workspace alive. This explicit ownership contract is an addition to the extraction, not a claim that the original native helper was zero-copy.

Begin a substep explicitly, integrate supplied bulk coefficients, read/patch only endpoints needed by consumer rules, solve ordered sweeps with explicit span/bounds, then request damping. Each phase commits independently; a rejected solve preserves the integrated state and all previously published multipliers. Splitting a solve does not reset lambda or alternating sweep order. `multipliers(edge_index)` returns that edge's lambda and stored span lambda; nil omits the edge. No force/payout policy is inferred. Drawing may directly consume bound p; complete snapshots still use `copy` into independent buffers. Bound state cannot also serve as coefficients, bounds, mobility or snapshot outputs. Bind may allocate during setup; warmed phases retain zero-allocation execution and private staging. Material conversion, coefficient generation, fixed-step scheduling, endpoint laws and the choice of phases remain consumer work. Use existing bulk operations to prepare changing channels rather than per-node Lua calls.

`scatter` stages and validates only its active write set, preserving original source values for self-overlap and leaving untouched positions unchanged. Its work scales with the supplied count rather than destination capacity. This does not change buffer precision or error/alias semantics.

Prepared rotations copy supplied segments, weights and the unnormalized axis. Endpoint reduction retains 18 moments and degree-17 arithmetic only when weight, angle, axis, count and magnitude satisfy its documented error predicate. Full output and inputs outside that domain use the full rotation loop. Shared constant rotation is an explicit request with zero bend. Shape construction, weight laws, pivots and any additional deformation remain in Lua.

An immutable geometry batch holds base vertices, topology, up to two copied weight channels and displacement directions. A pose caches weighted displacement, ordered rotation/scale/translation and optional explicitly parameterized planar perspective, before layers or colors. Applications choose when to evaluate and which layers share it. A new geometry needs a new pose; changing source buffers cannot mutate either. Each draw applies its own layer, offset, clip and colors through the existing raster path. No arbitrary expression evaluator, camera recipe, material rule or second framebuffer is introduced.

Projected polylines own copied points and optional scalar channels. Every draw prepares transient fragments from the current camera/plane/order/style inputs; there is no persistent projection/raster cache to invalidate. Original shared endpoints are projected once and reused even by crossing fragments. Gradient color is evaluated on original endpoints, once per distinct source/style, before splits or near clipping. Touching/coplanar sources use the explicitly supplied boundary style. RGB888 interpolation with floor then RGB565 quantization is intentionally different from existing palette blending. The original camera expression uses `{center_x,horizon,focal,eye_height,near_z}`, distinct from `geometry.project_points`; selecting camera coefficients and side semantics remains consumer work.

### Extraction mapping and verification

The extraction reference is revision `7df16b01`. The mapping below identifies loops, not whole former application functions. Test fixtures use independent chains, geometry and colors.

| Reference mechanism | Public execution | Consumer-owned composition |
| --- | --- | --- |
| `precise_float`, `refined_sqrt` | Private compensated helpers, reused `h2_f32_div` | Physical model and material conversion |
| `advance_rope` prepared coefficient and integration loops | Workspace load/begin/edge, integrate | Environment/drag law, released state, scheduling |
| `advance_rope` alternating local edges → span → bounds | Workspace solve with supplied records/ranges | Topology, payout, tension interpretation and positional rule selection |
| `advance_rope` neighbor then ordered axial displacement | Workspace damp | Whether/when to damp and supplied coefficients |
| `rod_pose` copied segments/moments, reduced endpoint and shared rotation | Prepared rotations | Shape/weights, root, calibrated load law, further deformation |
| `compile_geometry` / `draw_geometry` storage and array loops | Geometry batch → pre-layer pose → Display primitive replay | Channel construction, application projection parameters, sharing/layer scheduling |
| `depth_path` / `path_color` projection, split and raster loops | Display polyline with explicit plane and styles | Classification, fade coefficients, palette and draw order |
| `rod_step` | No extraction | Entire calibrated scalar model and scheduler |

The prepared translation units use `-O3 -fno-fast-math` on GNU-compatible toolchains, preserving explicit `fmaf`; the source-package dependency aspect exports the same flags per compilation unit. The independent package test checks those flags and compiles/loads the real built-ins. Existing Host numeric/raster harnesses cover complete state and pixel comparisons, failure atomicity, bounded storage, warm allocation, ownership and teardown; the existing browser fixture exercises the same standard Host. Numeric timing records setup, first execution, same-input native reference and warm phase bindings separately. Host timing is not a target FPS guarantee; paired downstream device evidence is still required before closing #383.
