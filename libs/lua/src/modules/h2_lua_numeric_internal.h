#ifndef H2_LUA_NUMERIC_INTERNAL_H
#define H2_LUA_NUMERIC_INTERNAL_H
#include "h2_lua_numeric.h"
#include "lauxlib.h"
#include "lua.h"
#include <math.h>
#include <string.h>
#define H2_NUMERIC_META "h2.numeric.buffer"
typedef struct h2_numeric_buffer {
  size_t count;
  int is_f32;
  /* Payload follows this header in the same userdata allocation. It has no
   * declared array type; typed stores establish its selected effective type.
   * The double member gives the header natural payload alignment. */
  union {
    float *f32;
    double *f64;
    double alignment;
  } data; /* values, then private scratch */
} h2_numeric_buffer_t;
h2_numeric_buffer_t *h2_numeric_check(lua_State *s, int at);
double h2_numeric_number(lua_State *s, int at);
size_t h2_numeric_size(lua_State *s, int at, size_t max);

void h2_numeric_capacity(lua_State *s, h2_numeric_buffer_t *b, size_t count);
/* Dispatch once; each typed check rejects heterogeneous operands, including
 * camera/index/mask/topology buffers, before any public write. */
#define H2_NUMERIC_DISPATCH(name, at)                                          \
  static int name(lua_State *s) {                                              \
    return h2_numeric_check(s, at)->is_f32 ? name##_f32(s) : name##_f64(s);    \
  }
#endif
