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
  double data[]; /* count values followed by count private scratch values. */
} h2_numeric_buffer_t;
h2_numeric_buffer_t *h2_numeric_check(lua_State *s, int at);
double h2_numeric_number(lua_State *s, int at);
size_t h2_numeric_size(lua_State *s, int at, size_t max);
void h2_numeric_commit(lua_State *s, h2_numeric_buffer_t *b, size_t count);
void h2_numeric_capacity(lua_State *s, h2_numeric_buffer_t *b, size_t count);
#endif
