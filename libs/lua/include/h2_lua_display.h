#ifndef H2_LUA_DISPLAY_H
#define H2_LUA_DISPLAY_H

/** @file h2_lua_display.h
 * @brief VM-owned procedural geometry shared by native producers and Display.
 *
 * Indexed rectangle Lua API (owning VM worker only):
 * - display.compile_rects(records) returns immutable userdata copied from a
 *   dense list of {x,y,width,height,color_index} named-field records. Integer
 *   x/y fit int32, width/height are 0..UINT32_MAX, indices are 1..16384.
 * - display.compile_palette(colors) copies a dense list of existing Display
 *   color strings or named {r=...,g=...,b=...} tables (integer 0..255) into
 *   fixed-length native RGB565 userdata. Both constructors allow 0..16384
 *   entries; inputs can be released after return and storage counts toward VM
 *   memory. Neither constructor itself opens or draws the Display.
 * - display.blend_palette(output,a,b,progress) requires equal palette lengths
 *   and integer progress 0..256. R5/G6/B5 channels use
 *   (a*(256-progress)+b*progress+128)>>8; endpoints are exact. Output can be
 *   either input. It returns no values, allocates nothing on success and does
 *   not require a live Display or change palette length.
 * - display.draw_rects(batch,palette[,left,top,right,bottom]) returns no values.
 *   Supply all four integer half-open clip bounds or omit all four for the full
 *   surface. Bounds must lie inside the framebuffer. Draw requires a live
 *   acquisition and validates every index, including clipped/empty rectangles,
 *   before writing. Later rectangles overwrite earlier ones. Successful calls
 *   allocate nothing, mark existing dirty/background damage and do not present.
 *
 * Existing stroke_path(points,widths,color,offset_x=0,top=0,bottom=height,
 * cache=false,fast=false,smooth=false,scale=1,tolerance=0) also accepts
 * points={buffer=xy,count=n}: xy is an f64 packed xy buffer, capacity>=2n,
 * n is an integer in 2..256, coordinates finite within +/-100000. Raw-read
 * descriptor fields; numeric table keys cannot coexist with a descriptor.
 * All other arguments, raster, clipping and (cache_hit,fast_segment_count)
 * returns are unchanged. Copy/validate current coordinates before drawing;
 * descriptor owns the existing normals cache, widths owns the span cache.
 * Values/count/style invalidate caches, never buffer identity alone. No
 * buffer pointer survives the call; roots survive getter/GC reentry, and
 * Display acquisition is rechecked before writes. Warm cache use allocates
 * nothing; cold cache/smooth scratch may allocate bounded VM storage.
 *
 * Prepared geometry Lua API (standard Host, owning VM worker only):
 * - display.draw_pose(pose,colors,origin_x,offset_x,offset_y,layer,scale,
 *   post_offset,left,top,right,bottom[,tint]) replays geometry.pose through
 *   the existing polygon/line raster. colors is f64 RGB565 per primitive;
 *   optional tint uses an existing Display color string/{r,g,b} named table.
 *   With projection, final coordinates are ((origin_x+x)+offset_x)*scale,
 *   ((y-layer*r)+offset_y)*scale; without it only x/y scale is applied.
 *   scale is (0,16]; post_offset shifts scanlines after polygon rasterization
 *   and line X before clipping. All scalars finite +/-1e6; final scaled xy
 *   within +/-1e6. Half-open integer clip bounds must fit the live surface.
 *   All colors/vertices validate before writes, even when clipped/tinted.
 *   Layer/color/clip changes do not invalidate or re-evaluate the pre-layer
 *   pose. Drawing uses its bounded scratch; no separate raster cache exists.
 * - display.polyline(capacity) -> batch: 0..256 points; VM-owned fixed storage
 *   for copied xyz, up to three scalar channels, shared endpoint projection
 *   and at most 2*(capacity-1) fragments (510 maximum). Constructors do not
 *   require an open Display. batch:load(points,n[,channels]) copies packed
 *   f64 xyz and optional packed scalar triples, n<=capacity; empty allowed.
 *   A successful load replaces all active data; rejected load preserves it.
 * - display.compile_line_style(values[,axis,reducer,operation]) -> immutable
 *   style: without axis, copy 0..255 f64 integer RGB565 source-segment colors.
 *   With axis, copy 11 f64 values {from_r,from_g,from_b,to_r,to_g,to_b,origin,
 *   coefficient,bias,lo,hi}; RGB888 channels in [0,255], 0<=lo<=hi<=1.
 *   axis 1..3 selects xyz, 4..6 selects a supplied scalar channel. reducer
 *   is "mean" or "min"; operation "multiply" or "divide" (nonzero divisor).
 *   For original source endpoints, value=reducer(a,b), delta=value-origin,
 *   t=clamp(delta*coefficient+bias,lo,hi), or delta/coefficient+bias. Each
 *   channel is floor(from*(1-t)+to*t), then RGB565 quantization. Fractional
 *   RGB888 endpoints are permitted. This differs from blend_palette math.
 * - display.draw_polyline(batch,camera,axis,offset,reverse,positive,negative,
 *   boundary,left,top,right,bottom): camera is f64 {center_x,horizon,focal,
 *   eye_height,near_z}; focal>0, near_z>=.001. Project
 *   (center_x+focal*x/z,horizon+focal*(eye_height-y)/z). This descriptor keeps
 *   its explicit expression order; it is not geometry.project_points layout.
 *   Split at xyz axis 1..3 = offset, strictly opposite signs only; touching
 *   or coplanar sources take boundary style. Other sources use a side only
 *   when both endpoints have its strict sign. Fragments retain source color,
 *   direction and side through near clipping; on-near is visible. reverse
 *   is required boolean, reversing source order only, never its endpoints
 *   or fragments. Shared original endpoints project once; repeated style
 *   references compute one color per source. Explicit styles must have n-1
 *   entries (zero for empty); selected channels must exist, even if hidden.
 *   All inputs/results validate before any pixel writes, including hidden
 *   styles and empty clips. Changed data/camera/plane/order/styles recompute
 *   transient fragments, so no cross-draw cache can become stale.
 *
 * Prepared draw/load/evaluate calls allocate nothing on success, never call
 * Lua per element, and retain no borrowed buffer/table/native pointers.
 * Colors' ordinary getters may reenter Lua; acquisition and pose validity
 * are checked after decoding. All drawing marks existing dirty/background
 * state and requires an open Display; it never implicitly presents.
 * Polyline payload costs 64*capacity bytes plus at most 510 fragment records
 * (under 48 KiB at maximum capacity), and fixed metadata/userdata overhead.
 * Styles cost 2 bytes per explicit color or 11 doubles plus fixed metadata.
 * GC/VM teardown owns cleanup, including partial constructor failure.
 *
 * Invalid arguments/acquisition raise Lua errors without partial operation
 * writes. OOM publishes no partial object; getters' own side effects remain
 * ordinary Lua behavior. GC/job/Host teardown reclaims userdata. Objects must
 * not cross VMs. No new caches, public pointers or close methods are exposed.
 * Existing commands/mesh contracts are unchanged. Standalone C applications
 * use h2_raster2d.h without a VM; its implementation-only span helper is not API.
 */
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_DISPLAY_VERTEX_LIMIT 65536u
#define H2_LUA_DISPLAY_PRIMITIVE_LIMIT 4096u

/** Finite screen-space coordinates in [-1000000, 1000000]. */
typedef struct h2_lua_display_vertex {
  double x;
  double y;
} h2_lua_display_vertex_t;

typedef enum h2_lua_display_primitive_kind {
  H2_LUA_DISPLAY_POLYGON = 0,
  H2_LUA_DISPLAY_LINE = 1,
} h2_lua_display_primitive_kind_t;

/** Ordered primitive referencing a contiguous range of the vertex array.
 * Polygons use 3..128 vertices; lines use exactly two. Ranges may overlap.
 */
typedef struct h2_lua_display_primitive {
  h2_lua_display_primitive_kind_t kind;
  size_t first; /**< Zero-based first vertex (Lua primitive tables are one-based). */
  size_t count;
  uint16_t color; /**< Native RGB565 value, not a serialized byte stream. */
} h2_lua_display_primitive_t;

/** Borrowed input arrays, stable throughout the call, including any GC during
 * push. NULL arrays are allowed only when their corresponding count is zero.
 * Arrays must not point into a retained batch's private storage.
 */
typedef struct h2_lua_display_mesh_data {
  const h2_lua_display_vertex_t *vertices;
  size_t vertex_count;
  const h2_lua_display_primitive_t *primitives;
  size_t primitive_count;
} h2_lua_display_mesh_data_t;

/** Fixed capacities, independently bounded by the two public limits. Initial
 * active lengths must fit. A zero-capacity empty batch is valid.
 */
typedef struct h2_lua_display_mesh_config {
  size_t vertex_capacity;
  size_t primitive_capacity;
  h2_lua_display_mesh_data_t initial;
} h2_lua_display_mesh_config_t;

/**
 * @brief Copy initial data and push one typed, VM-owned mesh userdata.
 * @param lua_state Current Lua state, borrowed within its owning native callback.
 * @param config Borrowed configuration; no pointers are retained.
 * @return OK pushes exactly one userdata. INVALID_ARG rejects malformed inputs;
 * NO_MEMORY reports protected allocation/stack-reservation failure;
 * INVALID_STATE reports other protected Lua failures. Failure preserves stack.
 *
 * Synchronous, owning worker only; not thread-safe or ISR-safe. This function
 * reserves its own scratch stack and protects Lua allocations from longjmp
 * across the caller's resource ownership. Geometry and transformed-coordinate
 * storage count toward VM memory and are reclaimed with the userdata/VM.
 * Neither this operation nor update opens, draws or presents the Display.
 * Keep the userdata referenced in Lua; never retain its internal native address.
 */
h2_pal_result_t h2_lua_display_mesh_push(
    void *lua_state, const h2_lua_display_mesh_config_t *config);

/**
 * @brief Transactionally replace active vertices, topology and colors.
 * @param lua_state Current state in the owning native callback.
 * @param stack_index Positive or relative stack index of a retained mesh;
 * zero, pseudo-indices and other userdata are rejected.
 * @param data Borrowed replacement arrays, copied only after full validation.
 * @return OK replaces data and invalidates derived coordinates/span caches. INVALID_ARG
 * leaves all prior data/cache state unchanged. Stack is unchanged on both paths.
 *
 * No allocation, stack growth, Lua callback or framebuffer access. Caller must
 * provide two unused stack slots (reserve with lua_checkstack before entering
 * a no-allocation section). The same worker/lifetime constraints as push apply.
 * Changes to active lengths, including empty, must fit the original capacities.
 */
h2_pal_result_t h2_lua_display_mesh_update(
    void *lua_state, int stack_index, const h2_lua_display_mesh_data_t *data);

#ifdef __cplusplus
}
#endif
#endif
