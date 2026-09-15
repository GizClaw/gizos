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
 * - display.draw_rects(batch,palette[,left,top,right,bottom]) returns no
 * values. Supply all four integer half-open clip bounds or omit all four for
 * the full surface. Bounds must lie inside the framebuffer. Draw requires a
 * live acquisition and validates every index, including clipped/empty
 * rectangles, before writing. Later rectangles overwrite earlier ones.
 * Successful calls allocate nothing, mark existing dirty/background damage and
 * do not present.
 *
 * Invalid arguments/acquisition raise Lua errors without partial operation
 * writes. OOM publishes no partial object; getters' own side effects remain
 * ordinary Lua behavior. GC/job/Host teardown reclaims userdata. Objects must
 * not cross VMs. No new caches, public pointers or close methods are exposed.
 * Existing commands/mesh contracts are unchanged. Standalone C applications
 * use h2_raster2d.h without a VM; its implementation-only span helper is not
 * API.
 *
 * Affine textures (owning VM worker only):
 * - display.texture(width,height,rgba) copies exactly width*height*4 bytes
 *   from a Lua string into immutable userdata. Dimensions are integers 1..4096;
 *   bytes are straight-alpha RGBA8, row-major, no padding. No image decoding.
 * - display.texture_batch(attachments,capacity) copies up to 256 attachment
 *   records and retains each texture. capacity is integer 0..4096 and never
 *   grows. Each record has texture, x, y, width, height, anchor_x, anchor_y.
 *   Atlas coordinates/sizes are integers 0..4096; rectangle must fit texture.
 *   Anchors are finite numbers within +/-1e6. Zero-area rectangles are valid.
 * - Numeric texture rows accept f32 or f64 storage; sampling uses double.
 * - display.update_textures(batch,buffer) transactionally publishes ordered
 *   rows {resource,a,b,c,d,tx,ty} from a vmath numeric buffer of exact 7*N
 *   length. resource is an integral one-based attachment index; N<=capacity.
 *   Matrix values are finite within +/-1e6; raster2d singular/inverse rules
 *   apply. Entire input is validated before publishing. Producer-owned batches
 *   (for example skeleton2d.textures) reject this manual update operation.
 * - display.draw_textures(batch[,left,top,right,bottom]) draws in batch order
 *   through h2_raster2d_draw_sprites. All four integer half-open clip bounds
 *   must be supplied together and lie inside the acquired framebuffer.
 *   Requires a live Display; does not present. A nonempty batch conservatively
 *   damages the entire clip, including transparent/singular attachments.
 *
 * Pixel centers, nearest sampling, atlas edges, full affine transforms,
 * straight-alpha blending and RGB565 quantization follow h2_raster2d.h.
 * Constructors do not acquire Display. Data and transaction scratch charge
 * the VM allocator; texture storage is a descriptor plus 4*width*height bytes;
 * batch storage is a header plus (resources+2*capacity) sprite descriptors,
 * plus Lua userdata/reference table overhead. Initialization temporarily also
 * retains the input string. Report actual VM/PAL peaks, not these sizes alone.
 * Keep batches referenced; they keep textures alive after source inputs are
 * released. GC and job/Host teardown reclaim storage. No public native pointer,
 * uncharged allocation, implicit image cache or explicit close operation.
 * Successful warmed update/draw calls allocate nothing. Lua errors preserve
 * the previously published batch/framebuffer for that operation; constructor
 * OOM publishes no object. Caller serializes use on its VM worker. Objects
 * cannot cross VMs. Existing present/PAL failure semantics are unchanged.
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
