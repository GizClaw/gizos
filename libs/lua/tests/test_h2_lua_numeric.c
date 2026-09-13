#include "h2_lua_numeric.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct allocation_counter {
  size_t calls, bytes, limit;
  int counting;
} allocation_counter_t;
static void *allocator(void *user, void *ptr, size_t old, size_t size) {
  allocation_counter_t *a = user;
  if (!ptr)
    old = 0;
  if (!size) {
    free(ptr);
    a->bytes -= old;
    return NULL;
  }
  if (a->counting)
    ++a->calls;
  if (size > a->limit - (a->bytes - old))
    return NULL;
  void *next = realloc(ptr, size);
  if (next)
    a->bytes = a->bytes - old + size;
  return next;
}
static void ok(lua_State *s, int status) {
  if (status != LUA_OK) {
    fprintf(stderr, "Lua numeric test: %s\n", lua_tostring(s, -1));
    abort();
  }
}
int main(void) {
  allocation_counter_t a = {.limit = 4 * 1024 * 1024};
  lua_State *s = lua_newstate(allocator, &a, 0);
  assert(s);
  luaL_requiref(s, LUA_GNAME, luaopen_base, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_LOADLIBNAME, luaopen_package, 1);
  lua_pop(s, 1);
  luaL_requiref(s, "vmath", h2_lua_open_vmath, 0);
  lua_pop(s, 1);
  luaL_requiref(s, "geometry", h2_lua_open_geometry, 0);
  lua_pop(s, 1);
  ok(s, luaL_loadfile(s, "libs/lua/tests/numeric.lua"));
  ok(s, lua_pcall(s, 0, 1, 0));
  assert(lua_isfunction(s, -1));
  /* Reserve stack and stop GC before counting allocations of a warmed call. */
  assert(lua_checkstack(s, 128));
  lua_gc(s, LUA_GCSTOP);
  lua_pushvalue(s, -1);
  ok(s, lua_pcall(s, 0, 0, 0));
  a.counting = 1;
  a.calls = 0;
  lua_pushvalue(s, -1);
  ok(s, lua_pcall(s, 0, 0, 0));
  a.counting = 0;
  assert(a.calls == 0);
  /* Constructor storage is charged to the same allocator, including scratch. */
  size_t before = a.bytes;
  ok(s, luaL_dostring(s, "retained=require('vmath').buffer(1024)"));
  assert(a.bytes >= before + 2 * 1024 * sizeof(double));
  ok(s, luaL_loadstring(s, "return require('vmath').buffer(65536)"));
  a.limit = a.bytes + 4096;
  assert(lua_pcall(s, 0, 1, 0) == LUA_ERRMEM);
  a.limit = 4 * 1024 * 1024;
  lua_close(s);
  assert(a.bytes == 0);
  puts("Lua numeric correctness, bounds, VM budget and zero-allocation tests "
       "passed");
  return 0;
}
