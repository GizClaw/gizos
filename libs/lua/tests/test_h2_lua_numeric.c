#include "h2_lua_numeric.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
/* Independent C oracle exercises the public Lua ABI and retained multipliers.
 * These two edges intentionally give their shared endpoint different weights.
 */
static void ordered_reference(lua_State *s) {
  double p[3] = {0, 2, 4}, lambda[2] = {-.02, -.01};
  const double wa[2] = {0, 3}, wb[2] = {5, 1};
  for (size_t pass = 0; pass < 4; ++pass) {
    for (size_t k = 0; k < 2; ++k) {
      size_t i = pass % 2 ? 1 - k : k;
      double d = p[i + 1] - p[i];
      double next =
          fmin(0, lambda[i] + (-(d - 1) - lambda[i]) / (wa[i] + wb[i] + 1));
      double delta = next - lambda[i];
      p[i] -= wa[i] * delta;
      p[i + 1] += wb[i] * delta;
      lambda[i] = next;
    }
  }
  ok(s, luaL_dostring(
            s, "local v=require('vmath'); local p=v.buffer(9); "
               "p:load({0,0,0,2,0,0,4,0,0}); "
               "local e=v.buffer(8); e:load({1,2,1,.0001,2,3,1,.0001}); "
               "local w=v.buffer(4); w:load({0,5,3,1}); "
               "local l=v.buffer(2); l:load({-.02,-.01}); "
               "for i=1,4 do v.relax_sweep(p,e,w,l,.01,3,2,i%2==0,true) end; "
               "return p:get(4),p:get(7),l:get(1),l:get(2)"));
  assert(fabs(lua_tonumber(s, -4) - p[1]) < 1e-12);
  assert(fabs(lua_tonumber(s, -3) - p[2]) < 1e-12);
  assert(fabs(lua_tonumber(s, -2) - lambda[0]) < 1e-12);
  assert(fabs(lua_tonumber(s, -1) - lambda[1]) < 1e-12);
  lua_pop(s, 4);
}
/* The float oracle uses the same physical equation in a one-dimensional
 * independent representation, with no production kernel/internal headers. */
static void float_reference(lua_State *s) {
  float p[3] = {0, 2, 4}, lambda[2] = {-.02f, -.01f};
  const float wa[2] = {0, 3}, wb[2] = {5, 1};
  float alpha = .0001f / (.01f * .01f);
  for (size_t pass = 0; pass < 4; ++pass) {
    for (size_t k = 0; k < 2; ++k) {
      size_t i = pass % 2 ? 1 - k : k;
      float distance = p[i + 1] - p[i];
      float next = fminf(0, lambda[i] + (-(distance - 1) - alpha * lambda[i]) /
                                            (wa[i] + wb[i] + alpha));
      float delta = next - lambda[i];
      p[i] -= wa[i] * delta;
      p[i + 1] += wb[i] * delta;
      lambda[i] = next;
    }
  }
  ok(s, luaL_dostring(
            s, "local v=require('vmath'); local function b(t) "
               "local b=v.buffer(#t,'f32'); b:load(t); return b end; "
               "local p=b({0,0,0,2,0,0,4,0,0}); "
               "local e=b({1,2,1,.0001,2,3,1,.0001}); "
               "local w=b({0,5,3,1}); local l=b({-.02,-.01}); "
               "for i=1,4 do v.relax_sweep(p,e,w,l,.01,3,2,i%2==0,true) end; "
               "return p:get(4),p:get(7),l:get(1),l:get(2)"));
  assert(fabs(lua_tonumber(s, -4) - (double)p[1]) < 1e-6);
  assert(fabs(lua_tonumber(s, -3) - (double)p[2]) < 1e-6);
  assert(fabs(lua_tonumber(s, -2) - (double)lambda[0]) < 1e-6);
  assert(fabs(lua_tonumber(s, -1) - (double)lambda[1]) < 1e-6);
  lua_pop(s, 4);
}
static void suite(lua_State *s, allocation_counter_t *a, const char *kind) {
  ok(s, luaL_loadfile(s, "libs/lua/tests/numeric.lua"));
  if (kind)
    lua_pushstring(s, kind);
  ok(s, lua_pcall(s, kind ? 1 : 0, 1, 0));
  assert(lua_isfunction(s, -1));
  assert(lua_checkstack(s, 128));
  lua_gc(s, LUA_GCSTOP);
  lua_pushvalue(s, -1);
  ok(s, lua_pcall(s, 0, 0, 0));
  a->counting = 1;
  a->calls = 0;
  lua_pushvalue(s, -1);
  ok(s, lua_pcall(s, 0, 0, 0));
  a->counting = 0;
  assert(a->calls == 0);
  lua_pop(s, 1);
}
static size_t buffer_charge(lua_State *s, allocation_counter_t *a, size_t count,
                            const char *kind) {
  lua_getglobal(s, "require");
  lua_pushliteral(s, "vmath");
  ok(s, lua_pcall(s, 1, 1, 0));
  lua_getfield(s, -1, "buffer");
  lua_remove(s, -2);
  lua_pushinteger(s, (lua_Integer)count);
  lua_pushstring(s, kind);
  size_t before = a->bytes;
  ok(s, lua_pcall(s, 2, 1, 0));
  size_t charge = a->bytes - before;
  lua_pop(s, 1);
  return charge;
}
/* Informational CPU timing only; host ratios do not predict MCU throughput. */
static void benchmark(lua_State *s, const char *kind) {
  ok(s,
     luaL_loadstring(
         s, "local kind=...; local v=require('vmath'); local n=64; "
            "local function b(n) return v.buffer(n,kind) end; "
            "local p,prev,acc,mass=b(3*n),b(3*n),b(3*n),b(n); "
            "local e,w,l=b(4*(n-1)),b(2*(n-1)),b(n-1); mass:fill(1); "
            "mass:set(1,0); w:fill(1); w:set(1,0); "
            "for i=1,n do p:set(3*i-2,(i-1)*1.01) end; prev:copy(p,1,1,3*n); "
            "for i=1,n-1 do e:set(4*i-3,i); e:set(4*i-2,i+1); "
            "e:set(4*i-1,1); e:set(4*i,.0001) end; "
            "return function() for i=1,2000 do "
            "v.verlet(p,prev,acc,mass,.01,1,n); "
            "v.relax_sweep(p,e,w,l,.01,n,n-1,i%2==0,false) end end"));
  lua_pushstring(s, kind);
  ok(s, lua_pcall(s, 1, 1, 0));
  lua_pushvalue(s, -1);
  ok(s, lua_pcall(s, 0, 0, 0));
  clock_t begin = clock();
  for (int i = 0; i < 5; ++i) {
    lua_pushvalue(s, -1);
    ok(s, lua_pcall(s, 0, 0, 0));
  }
  double ms = 1000.0 * (double)(clock() - begin) / CLOCKS_PER_SEC;
  printf("numeric benchmark %s: %.3f ms CPU, 10000 Verlet + relax_sweep calls, "
         "64 nodes/63 edges\n",
         kind, ms);
  lua_pop(s, 1);
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
  luaL_requiref(s, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(s, 1);
  luaL_requiref(s, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(s, 1);
  ordered_reference(s);
  float_reference(s);
  suite(s, &a, NULL);
  suite(s, &a, "f32");
  ok(s, luaL_dofile(s, "libs/lua/tests/numeric_f32.lua"));
  size_t f32_empty = buffer_charge(s, &a, 0, "f32");
  size_t f64_empty = buffer_charge(s, &a, 0, "f64");
  size_t f32 = buffer_charge(s, &a, 1024, "f32");
  size_t f64 = buffer_charge(s, &a, 1024, "f64");
  assert(f32_empty == f64_empty);
  assert(f32 - f32_empty == 2 * 1024 * sizeof(float));
  assert(f64 - f64_empty == 2 * 1024 * sizeof(double));
  assert(f64 - f32 == 8192);
  benchmark(s, "f32");
  benchmark(s, "f64");
  /* Constructor storage is charged to the same allocator, including scratch. */
  size_t before = a.bytes;
  ok(s, luaL_dostring(s, "retained=require('vmath').buffer(1024)"));
  assert(a.bytes >= before + 2 * 1024 * sizeof(double));
  ok(s, luaL_loadstring(s, "return require('vmath').buffer(65536)"));
  a.limit = a.bytes + 4096;
  assert(lua_pcall(s, 0, 1, 0) == LUA_ERRMEM);
  a.limit = 4 * 1024 * 1024;
  lua_pop(s, 1);
  ok(s, luaL_loadstring(s, "return require('vmath').buffer(65536,'f32')"));
  a.limit = a.bytes + 4096;
  assert(lua_pcall(s, 0, 1, 0) == LUA_ERRMEM);
  a.limit = 4 * 1024 * 1024;
  lua_close(s);
  assert(a.bytes == 0);
  puts("Lua numeric correctness, bounds, VM budget and zero-allocation tests "
       "passed");
  return 0;
}
