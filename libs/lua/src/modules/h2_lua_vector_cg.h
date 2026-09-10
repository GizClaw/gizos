#ifndef H2_LUA_VECTOR_CG_H
#define H2_LUA_VECTOR_CG_H
#include <stddef.h>
#include <stdint.h>
/* Native desktop backend. Output is transient premultiplied RGBA, not an asset. */
int h2_lua_vector_cg_render(const uint8_t *data, size_t length,
    uint8_t *rgba, unsigned width, unsigned height, const double matrix[6]);
#endif
