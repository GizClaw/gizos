#ifndef H2_LUA_NUMERIC_H
#define H2_LUA_NUMERIC_H
/** @file h2_lua_numeric.h
 * @brief Built-in `vmath` and `geometry` Lua modules (standard math is
 * unchanged).
 *
 * Host preloads both modules on every platform. These open functions use the
 * Lua C callback ABI: push one module table, return 1; may raise a Lua error.
 * Call only on the owning VM worker, inside a protected Lua invocation.
 * No Runtime/Display device is required. No native pointer escapes to Lua.
 *
 * Buffers hold binary64 (default "f64") or binary32 ("f32") values, fixed at
 * creation, plus equally sized private transactional scratch. Payload including
 * scratch costs 16 bytes per f64 element or 8 per f32 element, plus fixed
 * metadata/userdata overhead. All buffer operands in one call must have the same
 * kind, including coefficients, indices, masks, camera and mesh topology;
 * mismatches raise "mixed numeric buffer kinds (f32/f64)" even for empty calls.
 * All-f32 operations compute in float; scalar arguments are validated before
 * conversion to float once per call (load converts each imported element).
 * Parameter intervals use the selected precision. Values round on storage;
 * underflow may become zero. get/dot return ordinary Lua numbers; scalar-only
 * clamp/lerp/smoothstep/spring and standard math retain double semantics.
 * All storage uses Lua userdata and the VM allocator,
 * counts against vm_memory_limit_bytes, and is reclaimed by GC/VM destruction.
 * Values and results must be finite with absolute value <= 1e6. Sizes/indices
 * must be integers. Every misuse raises a Lua error; failed operations preserve
 * public buffer and mesh contents. Scratch contents are unspecified on error.
 * Successful operations allocate only at constructors/first require(); call
 * setup outside frame loops. Error construction can allocate. Numeric kernels
 * never yield, call Lua, access hardware, or retain external pointers.
 *
 * Lua API (arguments required unless marked optional, indices one-based):
 * - vmath.buffer(count[,kind]) -> b: zeroed fixed buffer, count in 0..65536;
 *   kind is "f64" (default, including nil) or "f32".
 * - #b -> count; b:get(index) -> number; b:set(index,value) -> nothing.
 * - b:fill(value) -> nothing: fill entire buffer.
 * - b:load(flat_table) -> nothing: raw dense prefix copy, suffix unchanged.
 * - dst:copy(src,dst_first,src_first,count) -> nothing: overlap-safe copy.
 * - vmath.clamp(x,lo,hi) -> number: closed interval, lo <= hi.
 * - vmath.lerp(a,b,t) -> number: a+(b-a)*t, permits extrapolation.
 * - vmath.smoothstep(lo,hi,x) -> number: clamped cubic, lo < hi.
 * - vmath.spring(x,v,target,k,c,accel,dt) -> x,v: semi-implicit Euler,
 *   v += (k*(target-x)-c*v+accel)*dt; x += v*dt; k,c >= 0, 1e-6 <= dt <= .1.
 * - vmath.combine(dst,a,b,ka,kb,bias,count) -> nothing: ka*a+kb*b+bias.
 * - vmath.polynomial(dst,src,coeff,count) -> nothing: Horner polynomial;
 *   coeff contains 1..9 ascending-power coefficients, including constant.
 * - vmath.clamp_bulk(dst,src,lo,hi,count) -> nothing: component clamp.
 * - vmath.gather(dst,src,first,stride,count) -> nothing: pack a strided
 *   scalar channel from src; stride >= 1, all source indices must fit.
 * - vmath.scatter(dst,src,first,stride,count) -> nothing: write a packed src
 *   prefix into strided dst indices; other destination values unchanged.
 * - vmath.multiply(dst,a,b,count) -> nothing: componentwise multiplication.
 * - vmath.divide(dst,a,b,count) -> nothing: componentwise division, zero
 *   denominator is an error even when the numerator is zero.
 * - vmath.length3(dst,src,n) -> nothing: xyz triples to n scalar lengths.
 * - vmath.normalize3(dst,src,n) -> nothing: xyz triples to unit vectors;
 *   zero vectors remain zero. Both vector operations allow n <= 21845.
 * - vmath.dot(a,b,count) -> number: dot product of scalar prefixes.
 * - vmath.verlet(p,prev,accel,inv_mass,dt,drag,n) -> nothing: packed xyz;
 *   p += (p-prev)/(1+drag*dt)+accel*dt^2, prev = old p. inv_mass zero pins
 *   both states; positive mass is a mobility flag (accel is already mass
 * scaled). n <= 256, drag >= 0, 1e-6 <= dt <= .1; writable p/prev must be
 * distinct from each other and from both input buffers.
 * - vmath.relax(p,inv_mass,edges,lambda,dt,iterations,n,m,tension_only)
 *   -> nothing: XPBD distance constraints; packed xyz, n in 1..256,
 *   m <= 512, iterations in 1..32, 1e-6 <= dt <= .1. Edge rows are
 *   {node_a,node_b,rest_length,compliance}; distinct valid node indices;
 *   rest/compliance/inv_mass >= 0, zero inverse mass pins nodes. Resets lambda
 *   prefix to zero each call; alternating forward/backward sweeps, skips
 *   coincident endpoints (<1e-12 distance) and fully pinned edges. With true,
 *   lambda <= 0 (tension only); false is bilateral. Writable p/lambda must
 *   be distinct from each other and every input. Read lambda/dt^2 as force.
 * - vmath.relax_sweep(p,edges,weights,lambda,dt,n,m,reverse,tension_only[,epsilon])
 *   -> nothing: one ordered XPBD sweep, retaining/updating lambda. Edge rows
 *   as relax; weights are per-edge {wa,wb}, nonnegative. n in 1..256,
 *   m <= 512, dt in [1e-6,.1]. reverse/tension_only are required booleans.
 *   epsilon defaults to 1e-12, must be nonnegative; skip distance < epsilon
 *   or zero distance or fully pinned edges. Writable p/lambda must differ
 *   from each other and edges/weights. No automatic lambda reset.
 * - vmath.damp_edges(p,prev,edges,weights,blend,threshold,epsilon,n,m)
 *   -> nothing: forward ordered per-edge prev update; same edge/weight layout,
 *   n in 1..256, m <= 512. For d >= rest*threshold and d > epsilon,
 *   remove blend of positive relative axial displacement, split by wa/(wa+wb)
 *   and wb/(wa+wb). Skip pinned edges; compliance validated but unused.
 *   blend in [0,1], threshold/epsilon >= 0; prev differs from all inputs.
 * - vmath.map(dst,src,operation,count) -> nothing: componentwise abs, sqrt,
 *   sin, cos or floor (string operation). Negative sqrt input raises error.
 * - vmath.select_le(dst,test,threshold,yes,no,count) -> nothing:
 *   dst[i] = test[i] <= threshold ? yes[i] : no[i], equality included.
 * - vmath.take(dst,src,indices,width,n) -> nothing: copy n indexed rows of
 *   width scalars, one-based row indices; width >= 1, all extents must fit.
 *   Duplicates, permutations and input/output aliasing supported.
 * - vmath.damp(p,prev,inv_mass,retain,blend,n) -> nothing: replace displacement
 *   with retain * lerp(displacement,neighbor_mean,blend), changing prev only.
 *   Endpoint mean is its own displacement; pinned prev unchanged; n <= 256;
 *   retain/blend in [0,1]; prev must differ from p and inv_mass.
 * - geometry.affine2(dst,src,matrix,n) -> nothing: packed xy, row-major 2x3.
 * - geometry.affine3(dst,src,matrix,n) -> nothing: packed xyz, row-major 3x4.
 * - geometry.displace3(dst,src,weights,dx,dy,dz,n) -> nothing: xyz += weight*d.
 * - geometry.rotate3(dst,src,weights,ax,ay,az,angle,n) -> nothing: Rodrigues
 *   around origin, normalized nonzero axis, radians angle*weight per vertex.
 * - geometry.prefix3(dst,segments,x,y,z,n) -> nothing: n xyz displacement
 *   vectors become n+1 points starting at (x,y,z).
 * - geometry.project_points(dst,mask,src,camera,n) -> nothing: xyz to xy;
 *   camera = {fx,fy,cx,cy,near}, near >= .001, +Z forward. Output is
 *   (cx+fx*x/z,cy+fy*y/z). z < near writes (0,0), mask 0; else mask 1.
 *   Negative fy supports screen Y inversion; camera positioning is affine3.
 * - geometry.segments(dst,points,n) -> n-1: packed xyz polyline (1..4097
 *   points) to independent {ax,ay,az,bx,by,bz} segment rows.
 * - geometry.project_segments(dst,ids,src,camera,n) -> visible_count:
 *   n <= 4096 six-value rows clipped at near (on-plane is visible), compacted
 *   four-value {ax,ay,bx,by} rows, IDs contain original segment indices.
 *   dst/ids must have room for n rows even when all segments are hidden.
 * - geometry.split_segments(dst,tags,src,axis,offset,n) -> piece_count:
 *   n <= 2048 six-value segment rows split at axis=1/2/3 coordinate=offset.
 *   Reserve 2*n rows in dst and tags. Tags are {side,source_index}, side
 *   -1/0/+1; zero only if entire segment is on plane. Touching endpoints
 *   do not add pieces. Original order and direction are preserved.
 * - geometry.mesh(vertex_capacity,primitive_capacity) -> writer,display_mesh:
 *   allocates reusable writer and public Display mesh, initially empty;
 *   vertex capacity <= 32768, primitive capacity <= 4096.
 * - geometry.update_mesh(writer,xy,topology,nv,np) -> same display_mesh:
 *   transactional update with packed xy and {kind,first,count,rgb565} rows;
 *   kind 0 polygon (3..128 vertices), 1 line (2 vertices); RGB565 0..65535.
 *   Active sizes must fit capacities. No drawing/presenting is performed.
 *
 * Prepared execution (all buffers explicitly f64; existing calls above are
 * unchanged). Constructors copy their inputs into bounded VM userdata. All
 * methods run synchronously without callbacks, allocations or yielding on
 * success. Inputs are finite +/-1e6; non-finite intermediates/results raise
 * errors. Each phase commits atomically; earlier successful phases remain.
 * Small values round/underflow in the explicitly selected float arithmetic.
 * All workspace input buffers in one call must be distinct, as must outputs.
 * - vmath.constraints(node_capacity,edge_capacity) -> workspace: capacities
 *   1..256 and 0..512. Unloaded methods except load raise errors.
 * - w:load(p,previous,edges,n,m,dt): packed xyz and six-value edge rows
 *   {a,b,rest,compliance,wa,wb}; a/b distinct indices in 1..n; other fields
 *   nonnegative. n in 1..capacity, m in 0..capacity, dt in [1e-6,.1]. Copies
 *   all active state, prepares rest squared/alpha/inverse denominators,
 *   resets all multipliers/sweep parity and disables the optional span.
 * - w:begin(dt): start a substep, reset edge/span lambda and sweep parity;
 *   changed dt refreshes coefficients. It does not integrate or change p/prev.
 * - w:node(index) -> x,y,z,px,py,pz; w:node(index,x,y,z,px,py,pz) patches
 *   one current/previous point. Reads do not copy the whole state.
 * - w:edge(index,a,b,rest,compliance,wa,wb) patches one edge and refreshes
 *   its coefficients without resetting lambda. w:span(a,b,rest,compliance,
 *   wa,wb) enables/patches the explicit span; w:span(nil) disables it.
 *   Span updates also retain lambda until begin/load.
 * - w:integrate(first,count,mobility,before,gain0,gain1,after,mode,bounds,nb):
 *   mobility has count values; each other coefficient buffer has 3*count.
 *   Nonnegative mobility is a flag: zero leaves p/previous unchanged.
 *   mode is "f64" or "displacement-f32". For enabled components use
 *   (old+(((old-previous+before)*gain0)*gain1))+after, then supplied bounds;
 *   previous becomes old. The float mode casts displacement, before and
 *   gains to float, keeps the two products separate, then adds the resulting
 *   increment to double old and adds double after. nb bounds rows each have
 *   {first,count,axis,lower,upper}, axis 1..3, lower<=upper, nb<=768. Bounds
 *   apply in order only to enabled nodes of the integrated range. nil requires
 *   nb=0. No environment, acceleration law or timestep scheduler is inferred.
 * - w:solve(iterations,bounds,nb): iterations 1..32; bounds as above.
 *   Each iteration runs forward/reverse alternating ordered edges, then the
 *   optional span, then positional bounds. Repeated solves retain lambda
 *   and parity. Local edges are tension-only, with binary64 world positions,
 *   two-float scratch, explicit fmaf, rationalized near-taut strain and float
 *   multipliers. Skip fully pinned edges, distances below 1e-8, and slack
 *   zero-lambda edges using gap < -1e-12f*rest_squared.hi. Span uses binary64
 *   distance with refined float sqrt seed (libm outside squared [1e-20,1e20])
 *   and acts only beyond its supplied rest length. Bounds compare leading
 *   float components and replace through two-float conversion; equal bounds
 *   pin a component. Bounds leave previous unchanged. No per-sweep callback.
 * - w:damp(mobility,blend,axial_blend,threshold,epsilon): n mobility flags;
 *   blends in [0,1], threshold/epsilon>=0. First blend internal enabled nodes'
 *   float displacements with their two neighbors; then update previous in
 *   supplied edge order for positive separating axial displacement when
 *   squared>=rest_f^2*threshold_f^2 and squared>epsilon_f^2. Edge endpoint
 *   weights, independently of neighbor mobility, determine axial sharing.
 *   Float displacement arithmetic updates double previous; p/lambda unchanged.
 * - w:copy(out_p,out_previous,out_lambda) -> span_lambda: copy all active
 *   state into distinct reusable f64 buffers; suffixes unchanged. Edge lambda
 *   is float promoted to double; span lambda is double. No mutable view
 * escapes. Workspace payload per capacity is 132 bytes/node plus two
 * prepared-edge records and 8 bytes/edge, with fixed object/userdata overhead.
 * It is bounded (under 160 KiB at maximum capacities) and reclaimed on GC/VM
 * teardown.
 *
 * - geometry.rotations(segments,weights,axis,n) -> rotations: copies n<=256
 *   xyz vectors, n scalar weights and a three-component nonzero supplied axis.
 *   Does not normalize axis. Inputs must differ. Prepares 18 moments and dot
 *   products once; no application shape/weight generation is included.
 * - r:evaluate(dst,angle,bend,yaw,x,y,z,full,shared) -> fast: accumulate
 *   Rodrigues(angle-bend*weight) vectors, then yaw, starting at x/y/z.
 *   full=true writes n+1 xyz points, otherwise one endpoint. shared=true
 *   requires bend=0 and composes one matrix before the vector loop. false
 *   shared/full can use degree-17 moment reduction: abs(weight)<=1,
 *   abs(bend)<=1.6, abs(angle)<=16, axis L1<=2, and a conservative remainder
 *   plus rounding bound <=1e-9 before translation; otherwise full evaluation.
 *   Return true only for that reduction. All outputs stage before publication.
 *   Payload is 40*n bytes plus fixed 18x3 moments/metadata; dst owns scratch.
 * - geometry.batch(xy,topology,w0,w1,directions,nv,np) -> immutable geometry:
 *   copied packed xy, topology {kind,first,count}, optional nil scalar weight
 *   channels and {dx0,dy0,dx1,dy1}. kind 0 polygon (3..128), 1 line (2).
 *   nv<=32768, np<=4096; empty allowed. No colors or application formulas.
 * - geometry.pose(batch) -> pose: owns a reference to immutable geometry;
 *   initially invalid. New geometry requires a new pose. Source buffer edits
 *   cannot change either object. Payload: geometry 32*nv plus topology;
 *   pose 64*nv bytes (published, staged, final draw coordinates) plus metadata.
 * - pose:evaluate(a0,a1,tx,ty,cos,sin,scale[,projection]) -> reused:
 *   add w0*a0*direction0 then w1*a1*direction1, rotate, scale, translate.
 *   Optional projection is {d0,d1,l0,l1,divisor,vertical_scale,origin_y}:
 *   depth=d0*u+d1*v, lateral=l0*u+l1*v, r=1/(1+depth/divisor),
 *   x=lateral*r, y=origin_y-(depth*vertical_scale)*r. Divisor/denominator
 *   must be nonzero, final r finite abs<=1000; negative denominators allowed.
 *   No projection publishes r=1. Cache key includes all scalar/projection
 *   inputs and immutable geometry identity, before any layer/draw style.
 *   Equal inputs return true; successful changed inputs increment generation.
 * - pose:copy(dst) -> generation: published {x,y,r} triples; initially invalid
 *   raises error. Failed evaluation preserves previous pose and cache validity.
 * Use display.draw_pose for direct primitive replay with separate layer/style.
 *
 * Count parameters use prefixes; remaining output values are unchanged.
 * Geometry transforms/projection are bounded by buffer capacity (affine2/3,
 * displace3/rotate3/project_points also n <= 21845; prefix3 n <= 21844).
 * Output buffers may alias inputs unless explicitly forbidden above; multiple
 * output buffers must always differ. No buffer views or resizes are provided.
 * Binary64 preserves smaller displacements; binary32 reduces storage and uses
 * single-precision arithmetic. Bulk calls amortize Lua overhead.
 * This API makes no target frame-rate or cross-platform bit identity promise.
 */
#define H2_LUA_NUMERIC_COUNT_LIMIT 65536u
#define H2_LUA_NUMERIC_VALUE_LIMIT 1000000.0
#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
int h2_lua_open_vmath(struct lua_State *state);
int h2_lua_open_geometry(struct lua_State *state);
#ifdef __cplusplus
}
#endif
#endif
