#ifndef H2_LUA_DISPLAY_H
#define H2_LUA_DISPLAY_H

/** @file h2_lua_display.h
 * @brief VM-owned procedural geometry shared by native producers and Display.
 *
 * Submission prototype (owning VM worker only):
 * - submit(options=nil) returns sequence, planned_pixels, planned_rectangles.
 *   Supports retained (default true), bounds, tiles and merge_gap as present does.
 *   tiles=true explicitly skips fine span planning and compares 16x16 tiles
 *   before the existing merge_gap/guard rules; omitted/false keeps the default.
 *   This trades planning CPU for potentially more submitted pixels. bounds=true
 *   still forces its tile-aligned union box. tiles is per-call and only affects
 *   retained planning; first/invalid frames stay full, unchanged frames empty.
 *   The sequence is scoped to this acquisition/job generation. At most one
 *   submission may be in flight, including active transport. A full mailbox
 *   returns nil, display.BUSY without enqueueing or changing drawing state.
 *   Runtime.sleep() may be used before retry; submit does not busy-spin.
 * - flush() polls the latest accepted submission, returning nil, display.BUSY
 *   while pending, nil, PAL_error on fault, or the same record as status().
 *   status() never waits for transport. completed counts finished attempts;
 *   successful counts successful attempts; changed_frames counts only actual
 *   changed successful frames. submitted is not a displayed-frame counter.
 *   started_us/completed_us are monotonic PAL timestamps for the latest
 *   completed attempt, usable only when clock_valid is true. pixels/rects
 *   count successful draw calls in that attempt. error latches the first
 *   fault; busy and closing describe the acquisition. completion_kind is
 *   transport for a qualified worker Host, pal_return for inline execution;
 *   neither proves scanout. No-change submits do not increment changed_frames.
 * - Without Host display_worker opt-in, submit executes inline with this same
 *   contract (including on Web). present/end_frame remain synchronous and
 *   return the current call's completed pixels/rectangles; they first drain
 *   any preceding submit. Existing callers that never submit are unchanged.
 * - Drawing can continue after successful submit; the worker uses one rooted
 *   immutable pixel snapshot and its own bounded plan/tile scratch. Snapshot
 *   preparation copies every planned rectangle at the original row stride;
 *   pixels outside submitted coverage need not be refreshed. Only after the
 *   full transport succeeds does the VM copy that same coverage to the retained
 *   baseline. First/invalid frames still copy the full frame.
 *   Snapshot, baseline and mailbox use VM quota. OOM raises a Lua error before
 *   publishing a new frame. First-use finalizer reentry can complete a nested
 *   submission; the outer preparation then returns BUSY without replacing its
 *   pending snapshot/plan. Closed or replaced acquisitions return an error.
 *   Task/atomic/semaphore storage is platform-owned.
 * - Faults stop further submissions; there is no automatic retry or backend
 *   recovery. deinit returns nil, BUSY/error until close and task join succeed;
 *   success keeps its existing no-values return. A faulted backend is not
 *   closed speculatively: its Host/VM/device lease remains quarantined. Keep
 *   dependencies alive and inspect host_destroy_checked rather than treating
 *   void host_destroy returning as permission to resume an external writer.
 *   The prototype has no recovery/unquarantine API. An unresponsive PAL can
 *   prevent shutdown indefinitely; no task deletion or hardware cancellation
 *   is attempted. The caller must retain the entire failed acquisition.
 *
 * String region Lua API (owning VM worker only):
 * - display.region_from_string(width,height,data,encoding="rgb565be") creates
 *   opaque region userdata for draw_region and full-screen restore_background.
 *   Dimensions are integers 1..4096. The data must be a string. Construction
 *   does not acquire or draw to Display and remains usable after deinit on an
 *   existing proxy. Initial require("display") still acquires Display and
 *   raises on acquisition failure; cached require does not reopen after deinit.
 *   Drawing still requires a live acquisition. Region storage counts toward
 *   the VM quota and is collected by Lua; the input is not retained.
 * - rgb565be is exactly width*height*2 row-major bytes, high byte first.
 * - rgb565be-lz4-b85 is eight hex digits of compressed byte length followed by
 *   Python base64.b85encode(block,pad=True) text for a standard raw LZ4 block.
 *   Padding must be zero, all input must be consumed, output must match the
 *   dimensions, and standard LZ4 final-sequence conditions apply. Malformed
 *   data raises a Lua error. Decoding uses the output userdata directly.
 *
 * Framebuffer capture Lua API (owning VM worker only):
 * - display.capture_region(x,y,width,height,key=nil,reuse=nil) captures an
 *   in-bounds RGB565 rectangle. Integer dimensions are 1..4096; x/y >= 0.
 *   A key trims only each row's outer margins and compiles non-key runs;
 *   interior key pixels remain available for opaque/different-key replay.
 * - Masked capture counts then allocates final VM storage directly. After
 *   allocation-triggered finalizers it revalidates the current acquisition,
 *   bounds and stride, and checks every pixel/run write against capacity.
 *   Changed totals use one stable VM snapshot fallback, never an unbounded
 *   retry or a framebuffer pointer retained across allocation. Finalizer
 *   effects remain ordinary Lua behavior; capture itself does not draw.
 * - Closed/out-of-bounds acquisition at validation raises an error. A copied
 *   fallback snapshot survives later Display closure. OOM publishes no partial
 *   result. All temporary/final userdata are VM-charged and GC-owned.
 * - Reuse requires identical dimensions and masking mode; masked reuse also
 *   requires the same RGB565 key. Mismatches raise an error. Opaque reuse, or
 *   masked reuse with sufficient existing pixel/run capacity, updates and
 *   returns the same userdata without allocation or finalizer callbacks. All
 *   aliases observe the updated content. Capacities do not shrink on reuse.
 *   Insufficient masked capacity uses the ordinary allocation path, returning
 *   a new region; the old region remains unchanged even on OOM (except for
 *   explicit mutations by user finalizers). Callers must retain the return
 *   value. No pool or extra capacity is allocated automatically.
 *   display.masked_region_reuse == true advertises this contract; older
 *   modules omit the field. It is informational like width/height: changing
 *   the Lua table field does not enable or disable native support.
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
 * Quad strip batches (owning VM worker only):
 * - display.compile_quad_batch(entries) copies 0..256 dense records
 *   {left,right,color_index[,top,bottom]} into immutable VM userdata.
 *   Fractions are finite, 0<=left<=right<=1 and 0<=top<=bottom<=1;
 *   color_index is an integer in 1..256. Equal endpoints remain valid and
 *   use the ordinary polygon raster, including inclusive horizontal spans.
 * - display.draw_quad_batch(batch,colors,ax,ay,bx,by,cx,cy,dx,dy,
 *   clip_top=0,clip_bottom=height) draws one quadrilateral's strips in record
 *   order. Omitted/nil clip bounds use the defaults; bounds are integers with
 *   0<=clip_top<=clip_bottom<=height, selecting half-open framebuffer rows.
 *   Clipping only limits raster rows; it never translates corners or changes
 *   interpolation/rounding. These row bounds are independent of the normalized
 *   top/bottom in batch records. Corners are finite +/-100000.
 *   Without top/bottom, vertices are A+(B-A)*left, A+(B-A)*right,
 *   D+(C-D)*right, D+(C-D)*left. With top/bottom, first form a transverse
 *   patch A'=A+(D-A)*top, B'=B+(C-B)*top, C'=B+(C-B)*bottom,
 *   D'=A+(D-A)*bottom, then apply the same strip formula to that patch.
 *   Each subtraction, multiplication and addition rounds separately as in
 *   Lua; explicit [0,1] patches are not simplified to direct strips.
 * - colors is an existing compile_palette handle covering all referenced
 *   indices, or an array of 0..256 existing Display colors. Array length must
 *   cover every index; all array colors decode before writes, even unused
 *   ones. A compiled palette uses its current RGB565 values without copying.
 *   Coordinate/index/color errors leave pixels untouched by this call;
 *   ordinary color getter side effects retain their Lua semantics. Display
 *   acquisition and clip are checked after getters, including for an empty
 *   batch or clip. An empty clip validates every input before returning.
 * - Draw returns no values, allocates no storage itself, borrows no data beyond
 *   return, and uses the existing polygon raster with full-width row clipping,
 *   dirty/background tracking and explicit present. Caller color getters can
 *   allocate/reenter; plain tables and palettes need no allocation. No mesh,
 *   expanded vertex buffer or span cache is created. Geometry, color recipes
 *   and topology selection remain the caller's responsibility.
 * - Constructors do not acquire Display; existing proxies can compile after
 *   deinit. Batch payload is two size_t fields plus 40 bytes per record on
 *   supported ABIs, charged to VM memory, with ordinary GC/teardown cleanup
 *   including failed construction. There are no retained registry roots.
 *
 * Two-rail material strips (experimental, owning VM worker only):
 * - display.material_strip(face_capacity) constructs bounded VM userdata;
 *   capacity is an integer 0..256. Initially empty and bound. No Display
 *   acquisition is required by construction, load or bind on an existing proxy.
 * - strip:load(first_xy_f64,last_xy_f64,station_count) copies two interleaved
 *   screen-space XY rails from existing vmath f64 buffers (capacity >= 2*count).
 *   Count is 0 or 2..face_capacity+1. Coordinates are finite +/-100000.
 *   Count N defines N-1 consecutive faces; closure requires an explicit final
 *   station. No topology, projection, interval or visibility policy is inferred.
 *   Each stored delta is last-first, rounded to binary64. Same-count load keeps
 *   bindings; a count change releases all bindings and requires bind for a
 *   nonempty strip. Failed load preserves the previous geometry and bindings.
 * - strip:bind(materials,palettes[,line_color_indices]) takes raw dense arrays
 *   with exactly one entry per active face. Materials are compile_quad_material
 *   handles; palettes are compile_palette handles covering every material index.
 *   Optional line indices are integers 0 (disabled) or 1..that palette's length;
 *   omission disables all lines. No color tables/getters are accepted. Complete
 *   validation precedes replacement. Strong userdata references retain handles
 *   and original fallback batches; the input arrays are not retained.
 * - display.draw_material_strip(strip,a,b,u_first,u_last,ratio,
 *   line_t=nil,clip_top=0,clip_bottom=height) returns (fast_faces,fallback_faces).
 *   a,b are finite [0,1], including reversed/equal intervals. Source U bounds
 *   satisfy 0<=first<=last<=1; ratio is finite and positive. Optional line_t is
 *   finite and between min(a,b) and max(a,b). Clip selects integer half-open
 *   rows inside the acquired framebuffer. Nil clip bounds use the defaults.
 *   Empty geometry/source/clip still validates inputs, binding and acquisition.
 * - Station evaluation is origin + delta*t, with each multiplication/addition
 *   rounded separately to binary64, even at t=1. Face i uses station i at a,b,
 *   then station i+1 at b,a. Each face draws its material followed immediately
 *   by its optional line before the next face. Material coverage, projective
 *   source cropping and complete original-batch fallback match the scalar
 *   draw_quad_material_projective API. Empty source/clip counts as fast.
 * - Lines use floor of the separately evaluated station coordinates, then the
 *   existing draw_line integer Bresenham phase. Row clips suppress writes,
 *   without restarting at clipped endpoints. Every enabled line endpoint must
 *   lie in [-width,2*width] x [-height,2*height], even for empty clips/source.
 *   nil line_t disables all lines. Empty source alone does not suppress a line.
 * - Each palette is read synchronously at draw time. Aliased mutable handles
 *   show the same current values in every referencing face: distinct colors in
 *   one strip draw need distinct handles. A palette may be blended/reused after
 *   the draw returns. No deferred commands, palette snapshots or registry roots.
 * - Successful load, bind and draw have no heap allocation or Lua callbacks
 *   after stack capacity is secured. Stack growth may run ordinary finalizers
 *   before mutable state/acquisition is read. Every generated corner and enabled
 *   line endpoint is validated before any framebuffer write; errors preserve
 *   pixels except ordinary finalizer side effects. Scratch is instance-owned;
 *   no allocation/reentry occurs during drawing. OOM construction publishes no
 *   partial object and is retryable. Storage is VM-charged, fixed at construction
 *   and reclaimed by GC/VM teardown, including bound handles when unreferenced.
 *   Draw uses existing dirty/background tracking and does not present. This is
 *   a CPU batching candidate, not an asynchronous submission or raster cache.
 *
 * Quad materials (opt-in parameter-space coverage, owning VM worker only):
 * - display.compile_quad_material(batch) precomposes an immutable quad batch
 *   into last-record-wins palette cells. Each normalized interval is half-open;
 *   zero-width/height records have no coverage. Unowned cells are transparent.
 *   At most 32 unique boundaries per axis (including 0 and 1) and 256 cells
 *   are accepted; excess complexity raises a Lua error before allocation.
 *   The material retains its source batch for fallback. Both are VM-charged
 *   userdata reclaimed by GC; there is no registry root or external allocator.
 * - display.draw_quad_material(material,colors,ax,ay,bx,by,cx,cy,dx,dy,
 *   clip_top=0,clip_bottom=height) has the same input validation, palette,
 *   callback, acquisition, row clip, damage and present rules as quad batches.
 *   Convex quads map the final cells bilinearly with integer-pixel samples.
 *   Grid-line X intercepts and row slopes are rounded to signed Q24 once at
 *   setup (nearest, ties away from zero); each row advances by that slope.
 *   Half-open parameter boundaries select the owner at shared edges. This
 *   differs from drawing separately rounded, inclusive polygon spans: callers
 *   explicitly opt in, and existing batch output is unchanged.
 * - Returns true when the material path handles the draw (including an empty
 *   clip/support), false when non-convex/degenerate or unsafe numeric geometry
 *   replays the original batch with its original raster rules and row clip.
 *   Setup rejects corner turn magnitudes below 1e-8, inconsistent turns, or
 *   X-intercept/slope envelopes above 1e9. Valid coordinates remain +/-100000.
 * - Drawing allocates no storage itself. Fixed call-local scratch is bounded
 *   by 64 grid edges and events. Compile work is O(records*cells); draw work
 *   is O(clipped_rows*boundaries^2 + written_pixels), independent of overdraw.
 *   Material payload is sizeof(private header) + 2*cells, at most 1056 bytes
 *   on the supported 32/64-bit ABIs, plus source batch and Lua object overhead.
 *   Constructors do not acquire Display; errors use ordinary Lua validation
 *   or quota/OOM errors. Geometry, palettes and cache policy stay with Lua.
 *
 * Cropped one-dimensional projective material mapping:
 * - display.draw_quad_material_projective(material,colors,
 *   ax,ay,bx,by,cx,cy,dx,dy,u_first,u_last,depth_ratio,
 *   clip_top=0,clip_bottom=height) uses an existing material and palette.
 *   All three mapping parameters are required. Source U bounds are finite
 *   0<=u_first<=u_last<=1; depth_ratio is finite and strictly positive.
 *   Equal U bounds draw nothing, but still validate all arguments, colors,
 *   row clip and live Display. V parameters are unchanged.
 * - The caller supplies corners of the projected cropped interval: A/D at
 *   u_first, B/C at u_last. For each source U boundary, clamp
 *   s=(u-u_first)/(u_last-u_first) to [0,1], then map it to target parameter
 *   t=s*r/(1+(r-1)*s), r=depth_ratio. For perspective depth use
 *   r=Z_last/Z_first, with same-sign endpoint depths and no horizon crossing.
 *   Stable equivalent arithmetic avoids intermediate overflow/cancellation;
 *   endpoints are exact. Lua owns projection, source/world units and palettes.
 * - Uses the same half-open cells, transparent holes, Q24 grid scan, row clip,
 *   dirty tracking and explicit present as draw_quad_material. Original cell
 *   ownership survives repeated/clipped grid boundaries. Ratio 1 is affine;
 *   (u_first,u_last,ratio)=(0,1,1) preserves the original draw exactly.
 * - Returns true on the material path (including empty crops/clips), false on
 *   fallback. Fallback intersects each original record with the U crop, skips
 *   empty U intervals, projects its endpoints and replays polygon records in
 *   order using the caller's original row clip. Exact identity delegates to
 *   the unchanged original fallback, including degenerate records.
 * - No new material, cache, atlas or allocation is created by a draw. Storage
 *   adds only three doubles of call-local mapping parameters to the existing
 *   bounded scan scratch; setup maps at most 32 U boundaries. Validation and
 *   color-getter lifecycle behavior are the same as draw_quad_material.
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
 * Smooth stroke coverage cache:
 * - With cache=true, smooth=true, a single color and tolerance=0, widths owns
 *   one immutable coverage entry separate from the hard-stroke span cache.
 *   It stores only quantized alpha, never color or framebuffer pixels; cold
 *   and hot draws preserve the uncached AA raster, blend order and damage.
 *   Color/background changes reuse coverage and blend into current pixels.
 * - The key includes point count, scaled coordinates/widths, scale, exact
 *   offset, row clip and viewport dimensions. Point/buffer identity alone is
 *   not a key. Changed values replace the entry; releasing widths permits GC.
 *   fast has no effect in smooth mode and does not invalidate coverage.
 * - Payload is sizeof(private header)+(3*n-1)*sizeof(double)+coverage_pixels,
 *   capped at 16 KiB per owner, excluding Lua overhead and existing job scratch.
 *   Both entry and scratch count toward VM quota. Multicolor, tolerance>0,
 *   oversized or empty bounds use the uncached path, report no hit, and keep
 *   any prior entry. Per-owner entries survive deinit while their owner lives;
 *   drawing requires reacquisition and revalidates the viewport and full key.
 * - Warm hits allocate nothing and return (true,0). Cold/fallback draws return
 *   (false,0). Invalid input or allocation failure does not draw or replace a
 *   complete entry. Allocation-driven Lua finalizers may reenter or close the
 *   Display; coverage is staged privately, acquisition rechecked, and only a
 *   complete entry is published before replay. Getter/finalizer side effects
 *   retain their normal Lua semantics. No new opacity or cache API is needed.
 *
 * Mesh draw extension (standard Host, owning VM worker only):
 * - display.draw_mesh(mesh,{transform={x=...,y=...,scale=...,angle=...},
 *   grid=...}) raw-reads named fields, exclusive with an explicit matrix. Require
 *   all four finite fields, x/y/angle in +/-100000, 0<scale<=100 and explicit
 *   integer grid in 1..16. Keep source order (x+(vx*cos-vy*sin)*scale)/grid
 *   and the y counterpart. Use the original conservative binary32 half-grid
 *   predicate (64*FLT_EPSILON bound), otherwise the original double expression;
 *   vertices outside +/-100000 use the double fallback. Round floor(q+.5)*grid.
 *   Final coordinates remain finite +/-16000000. Existing matrix arithmetic,
 *   raster, clipping, offset, color and explicit-present contracts remain.
 * - Successful updates revalidate derived data but retain the last valid span
 *   candidate. Reuse requires equal active counts, primitive kind/range/color,
 *   final coordinates, clip/viewport/offset/recolor and a valid complete cache.
 *   Different source/transform values may produce equal final coordinates.
 *   Failed operations preserve prior candidates; all final values validate
 *   before publishing derived results or pixels. Warm drawing allocates nothing.
 *   Cold retained identity drawing estimates 16..512 span records from clipped
 *   polygon row bounds and edge counts (one record per line), inspecting at
 *   most 128 polygon vertices. Larger/complex or transformed draws start at
 *   512. Each cache also owns one vertex/primitive snapshot at the declared
 *   mesh capacities, alignment and fixed metadata. Overflow draws completely
 *   without publishing partial spans; subsequent draws grow fourfold up to
 *   8192. A replayed cache with >=256 spare records shrinks to actual count;
 *   overflow after shrinking returns to 8192 and disables further shrinking.
 *   Initial estimation is only a hint: allocation finalizers may update mesh
 *   geometry or acquisition; normal revalidation and overflow rules remain.
 * - Nonidentity transforms of at most 1024 active vertices may stage once in
 *   independent VM-accounted storage shared by Display meshes. Reserve up to
 *   min(vertex_capacity,1024)*16 payload bytes plus userdata metadata, growing
 *   only at a cold capacity transition. Growth can temporarily retain both old
 *   and new allocations. Display release/VM teardown releases the shared root.
 *   Larger active meshes keep the original validation/transform passes; the
 *   public 65536-vertex limit is unchanged. No source, committed positions or
 *   complete span snapshot is borrowed as scratch. Reentrant allocation is
 *   followed by a fresh mesh/Display view, with no callbacks during publication.
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
 * Existing commands retain their contract. Standalone C applications
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
 * @return OK atomically replaces data and requires derived-coordinate revalidation.
 * Verified equivalent draw results may reuse prior span caches. INVALID_ARG
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
