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
 * Buffers hold binary64 values, fixed at creation, plus equally sized private
 * transactional scratch. All storage uses Lua userdata and the VM allocator,
 * counts against vm_memory_limit_bytes, and is reclaimed by GC/VM destruction.
 * Values and results must be finite with absolute value <= 1e6. Sizes/indices
 * must be integers. Every misuse raises a Lua error; failed operations preserve
 * public buffer and mesh contents. Scratch contents are unspecified on error.
 * Successful operations allocate only at buffer()/mesh()/first require(); call
 * setup outside frame loops. Error construction can allocate. Numeric kernels
 * never yield, call Lua, access hardware, or retain external pointers.
 *
 * Lua API (all arguments required, all indices one-based):
 * - vmath.buffer(count) -> b: zeroed fixed buffer, count in 0..65536.
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
 * Count parameters use prefixes; remaining output values are unchanged.
 * Geometry transforms/projection are bounded by buffer capacity (affine2/3,
 * displace3/rotate3/project_points also n <= 21845; prefix3 n <= 21844).
 * Output buffers may alias inputs unless explicitly forbidden above; multiple
 * output buffers must always differ. No buffer views or resizes are provided.
 * Binary64 preserves small displacements; bulk calls amortize Lua overhead.
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
