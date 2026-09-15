#ifndef H2_LUA_SKELETON2D_H
#define H2_LUA_SKELETON2D_H
/** @file h2_lua_skeleton2d.h
 * @brief VM-owned skeletal animation and existing Display mesh adapter.
 * All constructors copy inputs and charge the VM allocator. Hot operations do
 * not allocate. Errors raise Lua errors and preserve published pose/mesh.
 * All IDs are one-based except root parent=0. Tables are dense raw arrays.
 * Exactly one root is required at bone 1; every other parent precedes its
 * child. Definition limits and time/angle semantics follow h2_skeleton2d.h.
 * Lua channels 1..5 mean x, y, angle (radians), sx, sy respectively.
 * IDs and times in tables must be Lua integers; visible is integer 0 or 1,
 * layer is an integer in [-1000000,1000000], resource is 1..256.
 * Spatial numbers must be finite within +/-1000000. No numeric coercion.
 * compile({schema_version=1,bones,parts,clips}) -> definition.
 * bone={parent,x,y,angle,sx,sy};
 * part={bone,resource,layer,visible,x,y,angle,sx,sy}.
 * clip={duration_us=integer,tracks={...} }; track={bone=ID,channel=1..5,
 * linear=boolean,shortest=boolean,keys={ {time_us,value},...} }.
 * definition:instance() -> actor; definition:bytes() and actor:bytes() return
 * native userdata storage bytes, excluding Lua object overhead.
 * writer:bytes() includes its native resource/scratch arrays, but excludes
 * the separately owned Display mesh and Lua object/table overhead.
 * These byte queries are not total VM usage or allocator peak measurements.
 * actor:sample(clip,time_us,'clamp'|'repeat','current'|'a'|'b'); :blend(w).
 * The sample slot defaults to 'current'; blend weight must be in [0,1].
 * Sample/blend/set_local/set_parts/evaluate return no values. Pose slots
 * initially contain defaults; sample and sparse overrides remain pending
 * until evaluate publishes matrices and visible draw items.
 * actor:set_local(buffer): rows {bone,x,y,angle,sx,sy};
 * actor:set_parts(buffer): rows {part,resource,layer,visible}.
 * actor:evaluate({a,b,c,d,tx,ty}); :copy_matrices(buffer) -> bone count;
 * :copy_draw_items(buffer) -> visible count, rows {part,resource,layer,m[6]}.
 * skeleton2d.mesh(definition,resources,{vertices=N,primitives=N}) ->
 * writer,mesh.
 * resource={vertices={ {x,y},...},primitives={ {kind,first,count,rgb565},...}
 * }. Resource IDs index resources, at most 256; bounds derive from geometry.
 * Primitive kind 0 is a polygon of 3..128 vertices; kind 1 is a two-point line.
 * first is one-based and first+count-1 must fit the resource vertex array.
 * rgb565 is an integer in [0,65535]. Total resource storage is bounded by
 * 65536 vertices and 4096 primitives; output capacities use the same limits
 * and count all visible instances of a resource, not just unique resources.
 * Resource topology/vertices are copied at construction. Capacity cannot grow.
 * Copy methods write from buffer element 1 and preserve unused trailing
 * elements; they require vmath numeric buffers large enough for every output
 * row. Sparse input buffers contain exactly the supplied rows; duplicate IDs
 * fail. Buffer IDs must be integral. Reuse buffers and root_matrix in frame
 * loops. skeleton2d.update_mesh(writer,actor) -> same mesh, no drawing or
 * present. writer:copy_bounds(buffer) -> part count; rows
 * {visible,min_x,min_y,max_x,max_y}. Empty/hidden part rows are zero. Bounds
 * and mesh publish together. A writer and actor must reference the identical
 * definition; writer owns mesh and immutable resource copies. Only Lua's owning
 * worker may call these APIs. The returned mesh is draw-only: do not modify it
 * through another Display producer because unchanged poses reuse its contents.
 *
 * Texture attachment adapter:
 * skeleton2d.textures(definition,attachments) -> writer,batch.
 * Attachments use display.texture_batch records and one-based resource IDs;
 * metadata is copied and Display retains the immutable textures. Writer owns
 * definition and batch strongly. Batch capacity equals definition part count;
 * every visible draw item consumes one slot. No core C contract changes.
 * skeleton2d.update_textures(writer,actor) -> same batch, using published draw
 * items in stable layer/part order. Actor must have the identical definition.
 * Entire update validates resource IDs/matrices before publishing. Failure
 * preserves the previous batch; success allocates nothing and does not draw.
 * Caller draws with display.draw_textures(batch[,clip...]) then presents.
 * Returned batch is draw-only; display.update_textures rejects it.
 * The adapter stores no copied pixels, geometry or AABBs; Display owns texture
 * storage and transactional batch scratch. Resource/layer/visibility changes
 * follow actor evaluation. Singular transforms follow Raster2D rules. Authored
 * overlap/caps are application data, not automatic joint repair or skinning.
 */
#include "lua.h"
#ifdef __cplusplus
extern "C" {
#endif
int h2_lua_open_skeleton2d(lua_State *state);
#ifdef __cplusplus
}
#endif
#endif
