#ifndef H2_LUA_VECTOR_SW_H
#define H2_LUA_VECTOR_SW_H
#include <stddef.h>
#include <stdint.h>
/* Bounded, synchronous H2VG software renderer. Borrowed inputs; caller owns the
 * width * height * 4 output bytes (premultiplied RGBA). Zero means invalid
 * input or exhausted workspace. No retained state or allocations between calls.
 */
int h2_lua_vector_sw_render(const uint8_t *data, size_t length, uint8_t *rgba,
                            unsigned width, unsigned height,
                            const double matrix[6]);
#endif
