#ifndef H2_LUA_CANVAS_H
#define H2_LUA_CANVAS_H
#include "../runtime/h2_lua_internal.h"
/* Registers a scoped RGB888 composition surface on the display proxy.
 * begin_composite() imports the framebuffer; begin_composite(true) starts black.
 * begin_composite("retain") keeps the prior RGB888 composition, independent of
 * direct framebuffer edits. fade_composite(alpha) fades that retained surface.
 * end_composite() unions changed RGB565 bounds with existing display damage;
 * untouched frames require no upload. Retained additive primitives track 16x16
 * occupied/edited tiles; other drawing operations safely use full comparison.
 * Vector slices share an exact, lossless rendered-frame cache bounded to 512 KiB.
 * prepare_vector(resource, width, height) prepares contained vector artwork
 * with a transparent gutter and lossless RAM compression, bounded to 1.5 MiB
 * per job. Rendering reuses scratch storage to decompress. Returns retained
 * bytes, or nil/reason when the budget would be exceeded. Prepared bodies
 * survive reset_vector_cache(); all storage is released with the Lua job.
 * add_lines(commands, count [, scale, tx, ty]) accepts ordered nine-number
 * records {ax, ay, bx, by, width, r, g, b, alpha} in one flat reusable array.
 */
void h2_lua_canvas_register(lua_State *state, h2_lua_job_t *job);
void h2_lua_canvas_reset(lua_State *state);
#endif
