#include "h2_lua_numeric.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
/* Inject invalid stored coefficients to exercise phase validation separately
 * from buffer setters, which already reject those values. */
#include "../src/modules/h2_lua_numeric_internal.h"
/* Release toolchains must execute the oracle and allocation checks too. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct allocation_counter {
  size_t calls, bytes, limit;
  size_t attempts, fail_after;
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
  if (a->fail_after && ++a->attempts >= a->fail_after)
    return NULL;
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
/* Generic differential fixture derived from the extraction's compensated
 * loop, deliberately independent of production private headers. Its three
 * unequal edges, free endpoints and explicit bounds are not scene data. */
typedef struct reference_pair {
  float hi, lo;
} reference_pair_t;
static reference_pair_t reference_pair(double value) {
  float hi = (float)value;
  return (reference_pair_t){hi, (float)(value - hi)};
}
static reference_pair_t reference_add(reference_pair_t a, reference_pair_t b) {
  float sum = a.hi + b.hi, v = sum - a.hi;
  float error = (a.hi - (sum - v)) + (b.hi - v) + a.lo + b.lo;
  float hi = sum + error;
  return (reference_pair_t){hi, error - (hi - sum)};
}
static reference_pair_t reference_sub(reference_pair_t a, reference_pair_t b) {
  return reference_add(a, (reference_pair_t){-b.hi, -b.lo});
}
static reference_pair_t reference_square(reference_pair_t a) {
  float product = a.hi * a.hi,
        error = fmaf(a.hi, a.hi, -product) + 2.0f * a.hi * a.lo + a.lo * a.lo;
  float hi = product + error;
  return (reference_pair_t){hi, error - (hi - product)};
}
/* Keep the former span expression independent of prepared predicates and
 * snapshots. Recompute original pairs on every pass as the source did. */
static void span_reference(lua_State *s) {
  ok(s, luaL_loadstring(s,
      "local bound=...;local v=require('vmath');local p,prev=v.buffer(12),v.buffer(12);"
      "local w=v.constraints(4,0);local out,old,l=v.buffer(12),v.buffer(12),v.buffer(0);"
      "local bounds=v.buffer(5);bounds:load({1,4,2,.17,.17});"
      "p:fill(0);prev:fill(0);w[bound and 'bind' or 'load'](w,p,prev,l,4,0,.01);"
      "return function(values,a,b,wa,wb,dt,passes,clamp) "
      "if bound then p:load(values) else for i=1,4 do local k=3*i;"
      "w:node(i,values[k-2],values[k-1],values[k],0,0,0) end end;"
      "w:span(a,b,.4,.0001,wa,wb);w:begin(dt);"
      "w:solve(passes,clamp and bounds or nil,clamp and 1 or 0);"
      "local sl=w:copy(out,old,l);local result={};"
      "for i=1,12 do result[i]=out:get(i) end;return sl,result end"));
  /* Reuse each workspace across changed coordinates, endpoints, weights and
   * timesteps. Bound writes must refresh the span's original-coordinate view. */
  for (int bound = 0; bound <= 1; ++bound) {
    lua_pushvalue(s, -1);
    lua_pushboolean(s, bound);
    ok(s, lua_pcall(s, 1, 1, 0));
    const double weights[][2] = {{0, 1}, {3, 0}, {2, 5}, {-0.0, 0},
                                {1e-320, 1}, {1, 1e-320}};
    for (int round = 0; round < 72; ++round) {
      double p[12];
      reference_pair_t q[12];
      for (int i = 0; i < 12; ++i) {
        p[i] = (i + 1) / 7.0 + round / 13.0;
        q[i] = reference_pair(p[i]);
      }
      size_t a = (size_t)(round % 4), b = (a + 1 + round % 3) % 4;
      double wa = weights[round % 6][0], wb = weights[round % 6][1];
      double dt = round % 2 ? .02 : .01, lambda = 0;
      int passes = round % 3 == 0 ? 32 : 4, clamp = round % 2;
      for (int pass = 0; pass < passes; ++pass) {
        if (wa + wb != 0) {
          double av[3], bv[3], delta[3];
          for (int j = 0; j < 3; ++j) {
            size_t ai = 3 * a + j, bi = 3 * b + j;
            reference_pair_t ap = reference_pair(p[ai]), bp = reference_pair(p[bi]);
            av[j] = wa == 0 && q[ai].hi == ap.hi && q[ai].lo == ap.lo
                        ? p[ai] : (double)q[ai].hi + q[ai].lo;
            bv[j] = wb == 0 && q[bi].hi == bp.hi && q[bi].lo == bp.lo
                        ? p[bi] : (double)q[bi].hi + q[bi].lo;
            delta[j] = bv[j] - av[j];
          }
          double squared = delta[0] * delta[0] + delta[1] * delta[1] +
                           delta[2] * delta[2];
          double seed = (double)sqrtf((float)squared);
          double distance = squared < 1e-20 || squared > 1e20
                                ? sqrt(squared) : .5 * (seed + squared / seed);
          if (distance > .4) {
            double alpha = .0001 / (dt * dt);
            double dl = (- (distance - .4) - alpha * lambda) / (wa + wb + alpha);
            lambda += dl;
            double qa = wa * dl / distance, qb = wb * dl / distance;
            for (int j = 0; j < 3; ++j) {
              if (wa > 0) q[3 * a + j] = reference_pair(av[j] - qa * delta[j]);
              if (wb > 0) q[3 * b + j] = reference_pair(bv[j] + qb * delta[j]);
            }
          }
        }
        /* Equal bounds deliberately move even a span-pinned endpoint after
         * the first pass: matching only the weight would be incorrect. */
        if (clamp)
          for (int i = 0; i < 4; ++i) q[3 * i + 1] = reference_pair(.17);
      }
      lua_pushvalue(s, -1);
      lua_createtable(s, 12, 0);
      for (int i = 0; i < 12; ++i) {
        lua_pushnumber(s, p[i]); lua_rawseti(s, -2, i + 1);
      }
      lua_pushinteger(s, (lua_Integer)a + 1);
      lua_pushinteger(s, (lua_Integer)b + 1);
      lua_pushnumber(s, wa); lua_pushnumber(s, wb); lua_pushnumber(s, dt);
      lua_pushinteger(s, passes); lua_pushboolean(s, clamp);
      ok(s, lua_pcall(s, 8, 2, 0));
      assert(lua_tonumber(s, -2) == lambda);
      for (int i = 0; i < 12; ++i) {
        lua_rawgeti(s, -1, i + 1);
        assert(lua_tonumber(s, -1) == (double)q[i].hi + q[i].lo);
        lua_pop(s, 1);
      }
      lua_pop(s, 2);
    }
    lua_pop(s, 1);
  }
  lua_pop(s, 1);
}
static void prepared_reference(double *result) {
  double p[4][3] = {{0, .2, 0}, {.8, -.1, .1}, {1.9, .3, .2}, {2.8, .1, 0}};
  double prev[4][3] = {
      {0, .2, 0}, {.79, -.09, .09}, {1.88, .28, .19}, {2.77, .09, 0}};
  const double rest[3] = {.7, .9, .8}, compliance[3] = {.0001, .0002, .0003};
  const float wa[3] = {0, 3, 2}, wb[3] = {2, 1, 4};
  float alpha[3], inverse[3], lambda[3] = {0};
  reference_pair_t squared_rest[3], q[4][3];
  for (int i = 1; i < 4; ++i)
    for (int j = 0; j < 3; ++j) {
      double old = p[i][j];
      float delta = (float)(old - prev[i][j]);
      delta = (delta + .001f) * .9f;
      delta = delta * .8f;
      p[i][j] = (old + delta) + .002;
      prev[i][j] = old;
    }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 3; ++j)
      q[i][j] = reference_pair(p[i][j]);
  for (int i = 0; i < 3; ++i) {
    double a = compliance[i] / (.01 * .01);
    alpha[i] = (float)a;
    inverse[i] = 1.0f / (float)((double)wa[i] + wb[i] + a);
    squared_rest[i] = reference_pair(rest[i] * rest[i]);
  }
  double span_lambda = 0;
  for (int pass = 0; pass < 6; ++pass) {
    for (int k = 0; k < 3; ++k) {
      int i = pass % 2 ? 2 - k : k;
      reference_pair_t d[3];
      for (int j = 0; j < 3; ++j)
        d[j] = reference_sub(q[i + 1][j], q[i][j]);
      reference_pair_t squared = reference_add(
          reference_add(reference_square(d[0]), reference_square(d[1])),
          reference_square(d[2]));
      reference_pair_t difference = reference_sub(squared, squared_rest[i]);
      float gap = difference.hi + difference.lo;
      if (lambda[i] == 0 && gap < -1e-12f * squared_rest[i].hi)
        continue;
      float length = sqrtf(squared.hi);
      if (length < 1e-8f)
        continue;
      float strain = gap / (length + (float)rest[i]);
      float next =
          fminf(0, lambda[i] + (-strain - alpha[i] * lambda[i]) * inverse[i]);
      float scale = (next - lambda[i]) / length;
      lambda[i] = next;
      float a = wa[i] * scale, b = wb[i] * scale;
      for (int j = 0; j < 3; ++j) {
        if (wa[i] > 0)
          q[i][j] = reference_add(q[i][j], (reference_pair_t){-a * d[j].hi, 0});
        if (wb[i] > 0)
          q[i + 1][j] =
              reference_add(q[i + 1][j], (reference_pair_t){b * d[j].hi, 0});
      }
    }
    double delta[3], end[3];
    for (int j = 0; j < 3; ++j) {
      end[j] = (double)q[3][j].hi + q[3][j].lo;
      delta[j] = end[j] - p[0][j];
    }
    double squared =
        delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
    double seed = sqrtf((float)squared),
           distance = .5 * (seed + squared / seed);
    if (distance > 2.4) {
      double a = .0002 / (.01 * .01),
             dl = (-(distance - 2.4) - a * span_lambda) / (4 + a);
      span_lambda += dl;
      double scale = 4 * dl / distance;
      for (int j = 0; j < 3; ++j)
        q[3][j] = reference_pair(end[j] + scale * delta[j]);
    }
    for (int i = 1; i < 4; ++i)
      if (q[i][1].hi < 0)
        q[i][1] = reference_pair(0);
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 3; ++j)
      p[i][j] = (double)q[i][j].hi + q[i][j].lo;
  float displacement[4][3];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 3; ++j)
      displacement[i][j] = (float)(p[i][j] - prev[i][j]);
  for (int i = 1; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      prev[i][j] -=
          (double)(((displacement[i - 1][j] + displacement[i + 1][j]) * .5f -
                    displacement[i][j]) *
                   .3f);
  for (int i = 0; i < 3; ++i) {
    float d[3];
    for (int j = 0; j < 3; ++j)
      d[j] = (float)(p[i + 1][j] - p[i][j]);
    float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (squared > 1e-16f &&
        squared >= (float)rest[i] * (float)rest[i] * (.99f * .99f)) {
      float axial =
          (float)(p[i + 1][0] - prev[i + 1][0] - p[i][0] + prev[i][0]) * d[0] +
          (float)(p[i + 1][1] - prev[i + 1][1] - p[i][1] + prev[i][1]) * d[1] +
          (float)(p[i + 1][2] - prev[i + 1][2] - p[i][2] + prev[i][2]) * d[2];
      if (axial > 0) {
        float impulse = axial * .4f / (wa[i] + wb[i]) / squared,
              a = wa[i] * impulse, b = wb[i] * impulse;
        for (int j = 0; j < 3; ++j) {
          prev[i][j] -= (double)(a * d[j]);
          prev[i + 1][j] += (double)(b * d[j]);
        }
      }
    }
  }
  for (int i = 0; i < 12; ++i) {
    result[i] = p[i / 3][i % 3];
    result[12 + i] = prev[i / 3][i % 3];
  }
  for (int i = 0; i < 3; ++i)
    result[24 + i] = lambda[i];
  result[27] = span_lambda;
}
static void prepared_differential(lua_State *s, allocation_counter_t *a,
                                  int bound, int f32_coefficients) {
  double expected[28];
  prepared_reference(expected);
  clock_t prep = clock();
  size_t bytes = a->bytes;
  ok(s,
     luaL_loadstring(
         s,
         "local bound,kind=...;local v=require('vmath');local function b(t) local "
         "x=v.buffer(#t);x:load(t);return x end;"
         "local p=b{0,.2,0,.8,-.1,.1,1.9,.3,.2,2.8,.1,0};"
         "local prev=b{0,.2,0,.79,-.09,.09,1.88,.28,.19,2.77,.09,0};"
         "local e=b{1,2,.7,.0001,0,2,2,3,.9,.0002,3,1,3,4,.8,.0003,2,4};"
         "local mass=b{0,1,1,1};local "
         "before,g0,g1,after=v.buffer(12,kind),v.buffer(12,kind),v.buffer(12,kind),v.buffer("
         "12);"
         "before:fill(.001);g0:fill(.9);g1:fill(.8);after:fill(.002);"
         "local bounds=b{2,3,2,0,1e6};local w=v.constraints(4,3);"
         "local original,previous=v.buffer(12),v.buffer(12);"
         "original:copy(p,1,1,12);previous:copy(prev,1,1,12);"
         "if bound then w:bind(p,prev,e,4,3,.01) end;"
         "local out,old,lambda=v.buffer(12),v.buffer(12),v.buffer(3);"
         "return function(compare) if bound then p:copy(original,1,1,12);"
         "prev:copy(previous,1,1,12);w:begin(.01) else w:load(p,prev,e,4,3,.01) end;"
         "w:integrate(1,4,mass,before,g0,g1,after,'displacement-f32',nil,0);"
         "w:span(1,4,2.4,.0002,0,4);w:solve(6,bounds,1);w:damp(mass,.3,.4,.99,"
         "1e-8);"
         "local sl=w:copy(out,old,lambda);if compare then "
         "for i=1,12 do assert(math.abs(out:get(i)-compare[i])<2e-11);"
         "assert(math.abs(old:get(i)-compare[i+12])<2e-11) end;"
         "for i=1,3 do assert(math.abs(lambda:get(i)-compare[24+i])<2e-11) end;"
         "assert(math.abs(sl-compare[28])<2e-11) end end"));
  lua_pushboolean(s, bound);
  lua_pushstring(s, f32_coefficients ? "f32" : "f64");
  ok(s, lua_pcall(s, 2, 1, 0));
  printf("prepared fixture setup (%s, %s coefficients): %.3f ms CPU, %zu VM bytes\n",
         bound ? "bound" : "owned", f32_coefficients ? "f32" : "f64",
         1000.0 * (clock() - prep) / CLOCKS_PER_SEC, a->bytes - bytes);
  lua_pushvalue(s, -1);
  lua_createtable(s, 28, 0);
  for (int i = 0; i < 28; ++i) {
    lua_pushnumber(s, expected[i]);
    lua_rawseti(s, -2, i + 1);
  }
  clock_t first = clock();
  ok(s, lua_pcall(s, 1, 0, 0));
  printf("prepared first execution + full-state comparison: %.3f ms CPU\n",
         1000.0 * (clock() - first) / CLOCKS_PER_SEC);
  volatile double checksum = 0;
  clock_t raw = clock();
  for (int i = 0; i < 10000; ++i) {
    prepared_reference(expected);
    checksum += expected[27];
  }
  double reference_ms = 1000.0 * (clock() - raw) / CLOCKS_PER_SEC;
  clock_t warm = clock();
  a->counting = 1;
  a->calls = 0;
  for (int i = 0; i < 10000; ++i) {
    lua_pushvalue(s, -1);
    ok(s, lua_pcall(s, 0, 0, 0));
  }
  a->counting = 0;
  assert(a->calls == 0);
  printf("prepared same-input 10000 runs (%s, %s coefficients): native reference %.3f ms, Lua phase "
         "bindings %.3f ms CPU; "
         "4 nodes/3 edges/6 sweeps, %d native calls/run (including fixture reset "
         "and full oracle export), warm allocations %zu, "
         "checksum %.6f\n",
         bound ? "bound" : "owned", f32_coefficients ? "f32" : "f64", reference_ms,
         1000.0 * (clock() - warm) / CLOCKS_PER_SEC, bound ? 8 : 6, a->calls,
         (double)checksum);
  lua_pop(s, 1);
}
/* Fail every allocation position, including Lua's emergency-GC retry. A failed
 * rebind must retain its old ownership/results and leave the candidate free. */
static void binding_oom(lua_State *s, allocation_counter_t *a) {
  int failures = 0, successes = 0;
  for (size_t nth = 1; nth <= 12; ++nth) {
    lua_gc(s, LUA_GCCOLLECT);
    lua_gc(s, LUA_GCSTOP);
    ok(s, luaL_dostring(s,
        "local v=require('vmath');local function b(t) local a=v.buffer(#t);a:load(t);return a end;"
        "local p=b{0,0,0,2,0,0};local prev=b{0,0,0,1,0,0};"
        "local e=b{1,2,1,0,0,1};local w=v.constraints(2,1);"
        "w:bind(p,prev,e,2,1,.01);w:span(1,2,1,.0001,0,1);w:solve(1,nil,0);"
        "local x=w:node(2);local l,span=w:multipliers(1);"
        "local nextp=b{0,0,0,3,0,0};local nextprev=b{0,0,0,2,0,0};"
        "return function() w:bind(nextp,nextprev,e,2,1,.01) end,"
        "function(success) local probe=v.constraints(2,1);"
        "if success then assert(w:node(2)==3 and w:multipliers(1)==0);"
        "probe:bind(p,prev,e,2,1,.01) else "
        "assert(w:node(2)==x);local a,b=w:multipliers(1);assert(a==l and b==span);"
        "assert(not pcall(function() probe:bind(p,prev,e,2,1,.01) end));"
        "probe:bind(nextp,nextprev,e,2,1,.01);"
        "p:set(4,x+.1);assert(w:node(2)==x+.1) end end"));
    lua_pushvalue(s, -2);
    a->attempts = 0;
    a->fail_after = nth;
    int status = lua_pcall(s, 0, 0, 0);
    a->fail_after = 0;
    if (status != LUA_OK) {
      assert(status == LUA_ERRMEM);
      ++failures;
      lua_pop(s, 1);
    } else
      ++successes;
    lua_pushvalue(s, -1);
    lua_pushboolean(s, status == LUA_OK);
    ok(s, lua_pcall(s, 1, 0, 0));
    lua_pop(s, 2);
  }
  assert(failures >= 3 && successes > 0);
  printf("binding allocation failpoints: %d rolled back, %d successful\n",
         failures, successes);
}

/* Independent source-expression oracle: no production arithmetic header.
 * Cover the reachable refined/fallback boundary and logarithmic magnitudes.
 * The original upper fallback (>1e20 squared) is unreachable with +/-1e6
 * public inputs; input/output overflow is covered by the Lua rejection tests. */
static void refined_norm_reference(lua_State *s) {
  ok(s, luaL_dostring(s,
      "local v=require('vmath');local p,o=v.buffer(3),v.buffer(1);"
      "return function(x,y,z) p:set(1,x);p:set(2,y);p:set(3,z);"
      "v.length3_refined(o,p,1);return o:get(1) end"));
  double maximum_error = 0;
  for (int i = 0; i < 2048; ++i) {
    double x = ldexp(1.0 + (i % 31) / 31.0, i % 1070 - 1054),
           y = x * .37, z = -x * .71;
    if (i < 4) {
      const double boundary[] = {0, 1e-10, nextafter(1e-10, 0),
                                 nextafter(1e-10, 1)};
      x = boundary[i]; y = z = 0;
    }
    double squared = x * x + y * y + z * z, expected;
    if (squared < 1e-20 || squared > 1e20)
      expected = sqrt(squared);
    else {
      double seed = (double)sqrtf((float)squared);
      expected = .5 * (seed + squared / seed);
    }
    lua_pushvalue(s, -1);
    lua_pushnumber(s, x); lua_pushnumber(s, y); lua_pushnumber(s, z);
    ok(s, lua_pcall(s, 3, 1, 0));
    double actual = lua_tonumber(s, -1);
    assert(actual == expected);
    if (squared >= 1e-20 && squared <= 1e20) {
      double error = fabs(actual / sqrt(squared) - 1);
      assert(error <= 1e-12);
      if (error > maximum_error) maximum_error = error;
    }
    lua_pop(s, 1);
  }
  lua_pop(s, 1);
  printf("refined norm: 2048 source comparisons, max relative error %.3g\n",
         maximum_error);
}

/* Exercise the actual integration entrypoint with late invalid stored values,
 * including disabled nodes. The old double predicate is the acceptance oracle;
 * raw injection is private to this test and never exposed as a Lua binding. */
static void integration_coefficient_validation(lua_State *s) {
  const double values[] = {
      0, -0.0, FLT_MIN, -FLT_MIN, nextafterf(0, 1), -nextafterf(0, 1),
      1000000, -1000000, nextafterf(1000000, 0), nextafterf(-1000000, 0),
      nextafterf(1000000, INFINITY), nextafterf(-1000000, -INFINITY),
      nextafter(1000000, INFINITY), nextafter(-1000000, -INFINITY),
      INFINITY, -INFINITY, NAN};
  size_t cases = 0;
  for (int bound = 0; bound <= 1; ++bound) {
    for (int f32 = 0; f32 <= 1; ++f32) {
      for (int channel = 1; channel <= 3; ++channel) {
        int top = lua_gettop(s);
        ok(s, luaL_dostring(s,
            "return function(bound,kind,channel) local v=require('vmath');"
            "local p,prev=v.buffer(12),v.buffer(12);p:fill(91);prev:fill(92);"
            "p:load({0,0,0,1,0,0,2,0,0});prev:copy(p,1,1,9);"
            "local e=v.buffer(12);e:load({1,2,.9,.0001,0,1,2,3,.9,.0001,1,1});"
            "local w=v.constraints(3,2);"
            "if bound then w:bind(p,prev,e,3,2,.01) else w:load(p,prev,e,3,2,.01) end;"
            "w:span(1,3,1.8,.0001,0,1);w:solve(1,nil,0);"
            "local mobility=v.buffer(2);mobility:load({1,0});"
            "local inputs={v.buffer(9,kind),v.buffer(9,kind),v.buffer(9,kind)};"
            "inputs[2]:fill(1);inputs[3]:fill(1);"
            "local after=v.buffer(6);after:set(1,.01);"
            "local out,old,lambda=v.buffer(12),v.buffer(12),v.buffer(2);"
            "out:fill(93);old:fill(94);"
            "return inputs[channel],function(count) w:integrate(2,count,mobility,"
            "inputs[1],inputs[2],inputs[3],after,'displacement-f32',nil,0) end,"
            "function() return w:copy(out,old,lambda) end,out,old,lambda end"));
        lua_pushboolean(s, bound);
        lua_pushstring(s, f32 ? "f32" : "f64");
        lua_pushinteger(s, channel);
        ok(s, lua_pcall(s, 3, 6, 0));
        h2_numeric_buffer_t *input = h2_numeric_check(s, top + 1);
        h2_numeric_buffer_t *out = h2_numeric_check(s, top + 4);
        h2_numeric_buffer_t *old = h2_numeric_check(s, top + 5);
        h2_numeric_buffer_t *lambda = h2_numeric_check(s, top + 6);
        /* An invalid capacity suffix is not an active coefficient. */
        if (f32) input->data.f32[8] = NAN;
        else input->data.f64[8] = NAN;
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
          double value = f32 ? (double)(float)values[i] : values[i];
          if (f32) input->data.f32[5] = (float)value;
          else input->data.f64[5] = value;
          lua_pushvalue(s, top + 3);
          ok(s, lua_pcall(s, 0, 1, 0));
          double span = lua_tonumber(s, -1), saved[26];
          lua_pop(s, 1);
          memcpy(saved, out->data.f64, 12 * sizeof(double));
          memcpy(saved + 12, old->data.f64, 12 * sizeof(double));
          memcpy(saved + 24, lambda->data.f64, 2 * sizeof(double));
          lua_pushvalue(s, top + 2);
          lua_pushinteger(s, 2);
          int status = lua_pcall(s, 1, 0, 0);
          int accepted = isfinite(value) && fabs(value) <= 1000000.0;
          assert((status == LUA_OK) == accepted);
          if (!accepted) {
            assert(status == LUA_ERRRUN);
            assert(strstr(lua_tostring(s, -1),
                          "prepared numeric result out of bounds"));
            lua_pop(s, 1);
          }
          lua_pushvalue(s, top + 3);
          ok(s, lua_pcall(s, 0, 1, 0));
          assert(lua_tonumber(s, -1) == span);
          lua_pop(s, 1);
          assert(memcmp(saved + 24, lambda->data.f64, 2 * sizeof(double)) == 0);
          if (!accepted) {
            assert(memcmp(saved, out->data.f64, 12 * sizeof(double)) == 0);
            assert(memcmp(saved + 12, old->data.f64, 12 * sizeof(double)) == 0);
          } else {
            /* The disabled node and capacity suffix remain bit-identical. */
            assert(memcmp(saved + 6, out->data.f64 + 6, 6 * sizeof(double)) == 0);
            assert(memcmp(saved + 18, old->data.f64 + 6, 6 * sizeof(double)) == 0);
          }
          ++cases;
        }
        /* A zero-count operation still ignores all coefficient contents. */
        if (f32) input->data.f32[0] = NAN;
        else input->data.f64[0] = NAN;
        lua_pushvalue(s, top + 2);
        lua_pushinteger(s, 0);
        ok(s, lua_pcall(s, 1, 0, 0));
        lua_settop(s, top);
      }
    }
  }
  printf("integration coefficient validation: %zu typed boundary cases\n", cases);
}

static void suite(lua_State *s, allocation_counter_t *a, const char *kind,
                  const char *path) {
  ok(s, luaL_loadfile(s, path));
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
static void prepared_budget(lua_State *s, allocation_counter_t *a) {
  ok(s,
     luaL_dostring(
         s, "local v,g=require('vmath'),require('geometry');local "
            "seg,weights,axis=v.buffer(768),v.buffer(256),v.buffer(3);axis:set("
            "1,1);"
            "local "
            "xy,top,dir=v.buffer(1024),v.buffer(3),v.buffer(4);top:load{1,1,2};"
            "local geometry=g.batch(xy,top,nil,nil,dir,512,1);"
            "return {function() return v.constraints(256,512) end,"
            "function() return g.rotations(seg,weights,axis,256) end,"
            "function() return g.pose(geometry) end}"));
  for (int factory = 1; factory <= 3; ++factory) {
    for (size_t budget = 0; budget <= 2048; budget += 512) {
      lua_gc(s, LUA_GCCOLLECT);
      lua_gc(s, LUA_GCSTOP);
      lua_rawgeti(s, -1, factory);
      size_t baseline = a->bytes;
      a->limit = baseline + budget;
      assert(lua_pcall(s, 0, 1, 0) == LUA_ERRMEM);
      a->limit = 4 * 1024 * 1024;
      lua_pop(s, 1);
      lua_gc(s, LUA_GCCOLLECT);
      assert(a->bytes <= baseline + 1024);
    }
    lua_rawgeti(s, -1, factory);
    size_t baseline = a->bytes;
    ok(s, lua_pcall(s, 0, 1, 0));
    assert(a->bytes > baseline + 4096);
    printf("prepared constructor %d: %zu charged VM bytes\n", factory,
           a->bytes - baseline);
    lua_pop(s, 1);
  }
  lua_pop(s, 1);
  lua_gc(s, LUA_GCCOLLECT);
  lua_gc(s, LUA_GCSTOP);
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
  prepared_budget(s, &a);
  binding_oom(s, &a);
  for (int bound = 0; bound <= 1; ++bound)
    for (int f32 = 0; f32 <= 1; ++f32)
      prepared_differential(s, &a, bound, f32);
  refined_norm_reference(s);
  integration_coefficient_validation(s);
  span_reference(s);
  ordered_reference(s);
  float_reference(s);
  suite(s, &a, NULL, "libs/lua/tests/numeric.lua");
  suite(s, &a, "f32", "libs/lua/tests/numeric.lua");
  suite(s, &a, NULL, "libs/lua/tests/numeric_prepared.lua");
  suite(s, &a, NULL, "libs/lua/tests/geometry_prepared.lua");
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
