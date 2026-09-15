#include "h2_lua_numeric.h"
#include "../src/modules/h2_lua_texture_internal.h"
#include "h2_lua_skeleton2d.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct allocation_counter {
  size_t calls, bytes, limit, peak;
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
  if (next) {
    a->bytes = a->bytes - old + size;
    if (a->bytes > a->peak)
      a->peak = a->bytes;
  }
  return next;
}
static void ok(lua_State *s, int status) {
  if (status != LUA_OK) {
    fprintf(stderr, "Lua skeleton2d test: %s\n", lua_tostring(s, -1));
    abort();
  }
}
static void constructor_failures(void) {
  const size_t budgets[] = {1, 64, 256, 1024, 4096, 16384, 32768, 65536};
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    allocation_counter_t a = {.limit = 4 * 1024 * 1024};
    lua_State *s = lua_newstate(allocator, &a, 0);
    assert(s);
    luaL_requiref(s, LUA_GNAME, luaopen_base, 1);
    lua_pop(s, 1);
    luaL_requiref(s, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pop(s, 1);
    luaL_requiref(s, "skeleton2d", h2_lua_open_skeleton2d, 0);
    lua_pop(s, 1);
    ok(s, luaL_dostring(
              s, "local sk=require('skeleton2d');"
                 "local d=sk.compile{schema_version=1,bones={{0,0,0,0,1,1}},"
                 "parts={{1,1,0,1,0,0,0,1,1}},clips={}};"
                 "return function() local a=d:instance();return sk.mesh(d,"
                 "{{vertices={{0,0},{5,0},{5,5}},primitives={{0,1,3,65535}}}},"
                 "{vertices=1024,primitives=64}) end"));
    a.limit = a.bytes + budgets[i];
    int rc = lua_pcall(s, 0, 0, 0);
    assert(rc == LUA_OK || rc == LUA_ERRMEM || rc == LUA_ERRRUN);
    if (budgets[i] == 1)
      assert(rc != LUA_OK);
    a.limit = 4 * 1024 * 1024;
    lua_close(s);
    assert(a.bytes == 0);
  }
}
int main(void) {
  constructor_failures();
  allocation_counter_t a = {.limit = 4 * 1024 * 1024};
  lua_State *s = lua_newstate(allocator, &a, 0);
  assert(s);
  luaL_requiref(s, LUA_GNAME, luaopen_base, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_LOADLIBNAME, luaopen_package, 1);
  lua_pop(s, 1);
  luaL_requiref(s, "vmath", h2_lua_open_vmath, 0);
  lua_pop(s, 1);
  luaL_requiref(s, "skeleton2d", h2_lua_open_skeleton2d, 0);
  lua_pop(s, 1);
  lua_pushcfunction(s, h2_lua_texture_new);
  lua_setglobal(s, "test_texture_new");
  ok(s, luaL_loadfile(s, "libs/lua/tests/skeleton2d.lua"));
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
  printf("Lua skeleton2d raw VM peak_bytes=%zu steady_allocations=%zu\n",
         a.peak, a.calls);
  lua_close(s);
  assert(a.bytes == 0);
  puts(
      "Lua skeleton2d correctness, bounds, VM budget and zero-allocation tests "
      "passed");
  return 0;
}
