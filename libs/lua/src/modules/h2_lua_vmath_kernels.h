/* Intentionally included twice: one algorithm, two element types.
 * NUM_* is configured by h2_lua_numeric_typed.h. */
#include "h2_lua_numeric_typed.h"

static int NUM_NAME(buffer_get)(lua_State *s) {
  h2_numeric_buffer_t *b = NUM_NAME(buffer_check)(s, 1);
  lua_pushnumber(s, (lua_Number)NUM_DATA(b)[index_of(s, 2, b->count)]);
  return 1;
}
static int NUM_NAME(buffer_set)(lua_State *s) {
  h2_numeric_buffer_t *b = NUM_NAME(buffer_check)(s, 1);
  size_t i = index_of(s, 2, b->count);
  NUM_REAL v = NUM_NAME(number)(s, 3);
  NUM_DATA(b)[i] = v;
  return 0;
}
static int NUM_NAME(buffer_fill)(lua_State *s) {
  h2_numeric_buffer_t *b = NUM_NAME(buffer_check)(s, 1);
  NUM_REAL v = NUM_NAME(number)(s, 2);
  for (size_t i = 0; i < b->count; ++i)
    NUM_DATA(b)[i] = v;
  return 0;
}
static int NUM_NAME(buffer_load)(lua_State *s) {
  h2_numeric_buffer_t *b = NUM_NAME(buffer_check)(s, 1);
  luaL_checktype(s, 2, LUA_TTABLE);
  size_t n = lua_rawlen(s, 2);
  h2_numeric_capacity(s, b, n);
  for (size_t i = 0; i < n; ++i) {
    lua_rawgeti(s, 2, (lua_Integer)i + 1);
    NUM_DATA(b)[b->count + i] = NUM_NAME(number)(s, -1);
    lua_pop(s, 1);
  }
  NUM_NAME(commit)(s, b, n);
  return 0;
}
static int NUM_NAME(buffer_copy)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *b = NUM_NAME(buffer_check)(s, 2);
  size_t di = index_of(s, 3, d->count), bi = index_of(s, 4, b->count);
  size_t n = h2_numeric_size(s, 5, b->count - bi);
  h2_numeric_capacity(s, d, di + n);
  memmove(NUM_DATA(d) + di, NUM_DATA(b) + bi, n * sizeof(NUM_REAL));
  return 0;
}
/* Strided channel transfer lets Lua compose bulk formulas on interleaved
 * coordinates without crossing the native boundary for every vertex. */
static int NUM_NAME(channel)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *src = NUM_NAME(buffer_check)(s, 2);
  int scatter = (int)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_buffer_t *strided = scatter ? d : src, *packed = scatter ? src : d;
  size_t first = index_of(s, 3, strided->count),
         stride = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT);
  size_t n = h2_numeric_size(s, 5, packed->count);
  if (!stride || (n && (n - 1) > (strided->count - 1 - first) / stride))
    return luaL_error(s, "strided range out of bounds");
  if (scatter) {
    /* Snapshot only the write set, before publishing any overlapping source. */
    for (size_t i = 0; i < n; ++i)
      NUM_DATA(d)[d->count + i] = NUM_NAME(checked)(s, NUM_DATA(src)[i]);
    for (size_t i = 0; i < n; ++i)
      NUM_DATA(d)[first + i * stride] = NUM_DATA(d)[d->count + i];
  } else {
    for (size_t i = 0; i < n; ++i)
      NUM_DATA(d)[d->count + i] = NUM_DATA(src)[first + i * stride];
    NUM_NAME(commit)(s, d, n);
  }
  return 0;
}
/* All bulk operations read public values and publish from VM-owned scratch.
 * This permits aliasing and preserves every output on numeric overflow. */
static int NUM_NAME(combine)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *a = NUM_NAME(buffer_check)(s, 2),
                      *b = NUM_NAME(buffer_check)(s, 3);
  NUM_REAL ka = NUM_NAME(number)(s, 4), kb = NUM_NAME(number)(s, 5),
           bias = NUM_NAME(number)(s, 6);
  size_t n = h2_numeric_size(s, 7, d->count);
  h2_numeric_capacity(s, a, n);
  h2_numeric_capacity(s, b, n);
  for (size_t i = 0; i < n; ++i)
    NUM_DATA(d)
    [d->count + i] = ka * NUM_DATA(a)[i] + kb * NUM_DATA(b)[i] + bias;
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(polynomial)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *a = NUM_NAME(buffer_check)(s, 2),
                      *c = NUM_NAME(buffer_check)(s, 3);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  if (!c->count || c->count > 9)
    return luaL_error(s, "polynomial requires 1..9 coefficients");
  for (size_t i = 0; i < n; ++i) {
    NUM_REAL v = 0;
    for (size_t j = c->count; j > 0; --j)
      v = v * NUM_DATA(a)[i] + NUM_DATA(c)[j - 1];
    NUM_DATA(d)[d->count + i] = v;
  }
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(clamp_bulk)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *a = NUM_NAME(buffer_check)(s, 2);
  NUM_REAL lo = NUM_NAME(number)(s, 3), hi = NUM_NAME(number)(s, 4);
  size_t n = h2_numeric_size(s, 5, d->count);
  h2_numeric_capacity(s, a, n);
  if (lo > hi)
    return luaL_error(s, "reversed interval");
  for (size_t i = 0; i < n; ++i)
    NUM_DATA(d)
    [d->count + i] = NUM_MATH(fmax)(lo, NUM_MATH(fmin)(hi, NUM_DATA(a)[i]));
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(product)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *a = NUM_NAME(buffer_check)(s, 2),
                      *b = NUM_NAME(buffer_check)(s, 3);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  h2_numeric_capacity(s, b, n);
  int divide = (int)lua_tointeger(s, lua_upvalueindex(1));
  for (size_t i = 0; i < n; ++i) {
    if (divide && NUM_DATA(b)[i] == 0)
      return luaL_error(s, "division by zero");
    NUM_DATA(d)
    [d->count + i] = divide ? NUM_DATA(a)[i] / NUM_DATA(b)[i]
                            : NUM_DATA(a)[i] * NUM_DATA(b)[i];
  }
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(vector_length)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2);
  size_t n = h2_numeric_size(s, 3, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  int normalize = (int)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, d, normalize ? 3 * n : n);
  for (size_t i = 0; i < n; ++i) {
    const NUM_REAL *v = NUM_DATA(p) + 3 * i;
    /* Bounded inputs cannot overflow the squared norm. Scale tiny vectors
     * before normalization so even subnormal components produce unit vectors.
     */
    NUM_REAL squared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    NUM_REAL scale = 1, unit[3] = {v[0], v[1], v[2]};
    if (squared < NUM_TINY_SQUARED) {
      scale = NUM_MATH(fmax)(
          NUM_MATH(fabs)(v[0]),
          NUM_MATH(fmax)(NUM_MATH(fabs)(v[1]), NUM_MATH(fabs)(v[2])));
      if (scale > 0) {
        for (size_t j = 0; j < 3; ++j)
          unit[j] /= scale;
      }
      squared = unit[0] * unit[0] + unit[1] * unit[1] + unit[2] * unit[2];
    }
    NUM_REAL len = NUM_MATH(sqrt)(squared);
    if (normalize) {
      for (size_t j = 0; j < 3; ++j)
        NUM_DATA(d)[d->count + 3 * i + j] = len == 0 ? 0 : unit[j] / len;
    } else
      NUM_DATA(d)[d->count + i] = scale * len;
  }
  NUM_NAME(commit)(s, d, normalize ? 3 * n : n);
  return 0;
}
static int NUM_NAME(dot)(lua_State *s) {
  h2_numeric_buffer_t *a = NUM_NAME(buffer_check)(s, 1),
                      *b = NUM_NAME(buffer_check)(s, 2);
  size_t n = h2_numeric_size(s, 3, a->count);
  h2_numeric_capacity(s, b, n);
  NUM_REAL v = 0;
  for (size_t i = 0; i < n; ++i)
    v += NUM_DATA(a)[i] * NUM_DATA(b)[i];
  lua_pushnumber(s, (lua_Number)NUM_NAME(checked)(s, v));
  return 1;
}
static int NUM_NAME(verlet)(lua_State *s) {
  h2_numeric_buffer_t *p = NUM_NAME(buffer_check)(s, 1),
                      *prev = NUM_NAME(buffer_check)(s, 2),
                      *acc = NUM_NAME(buffer_check)(s, 3),
                      *weights = NUM_NAME(buffer_check)(s, 4);
  NUM_REAL dt = NUM_NAME(number)(s, 5), drag = NUM_NAME(number)(s, 6);
  size_t n = h2_numeric_size(s, 7, 256);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, acc, 3 * n);
  h2_numeric_capacity(s, weights, n);
  if (p == prev || p == acc || p == weights || prev == acc || prev == weights ||
      dt < NUM_C(1e-6) || dt > NUM_C(.1) || drag < 0)
    return luaL_error(s, "invalid Verlet buffers or parameters");
  NUM_REAL factor = 1 / (1 + drag * dt);
  for (size_t i = 0; i < n; ++i) {
    if (NUM_DATA(weights)[i] < 0)
      return luaL_error(s, "negative inverse mass");
    for (size_t j = 3 * i; j < 3 * i + 3; ++j) {
      NUM_DATA(p)
      [p->count + j] =
          NUM_DATA(weights)[i] == 0
              ? NUM_DATA(p)[j]
              : NUM_DATA(p)[j] + (NUM_DATA(p)[j] - NUM_DATA(prev)[j]) * factor +
                    NUM_DATA(acc)[j] * dt * dt;
      NUM_NAME(checked)(s, NUM_DATA(p)[p->count + j]);
    }
  }
  for (size_t i = 0; i < n; ++i)
    if (NUM_DATA(weights)[i] > 0)
      memcpy(NUM_DATA(prev) + 3 * i, NUM_DATA(p) + 3 * i, 3 * sizeof(NUM_REAL));
  NUM_NAME(commit)(s, p, 3 * n);
  return 0;
}
static int NUM_NAME(relax)(lua_State *s) {
  h2_numeric_buffer_t *p = NUM_NAME(buffer_check)(s, 1),
                      *w = NUM_NAME(buffer_check)(s, 2),
                      *edges = NUM_NAME(buffer_check)(s, 3),
                      *lambda = NUM_NAME(buffer_check)(s, 4);
  NUM_REAL dt = NUM_NAME(number)(s, 5);
  size_t iterations = h2_numeric_size(s, 6, 32), n = h2_numeric_size(s, 7, 256),
         m = h2_numeric_size(s, 8, 512);
  luaL_checktype(s, 9, LUA_TBOOLEAN);
  int tension = lua_toboolean(s, 9);
  if (!iterations || !n || dt < NUM_C(1e-6) || dt > NUM_C(.1) || p == w ||
      p == edges || p == lambda || lambda == w || lambda == edges)
    return luaL_error(s, "invalid constraint parameters or aliases");
  h2_numeric_capacity(s, p, n * 3);
  h2_numeric_capacity(s, w, n);
  h2_numeric_capacity(s, edges, m * 4);
  h2_numeric_capacity(s, lambda, m);
  for (size_t i = 0; i < n; ++i)
    if (NUM_DATA(w)[i] < 0)
      return luaL_error(s, "negative inverse mass");
  for (size_t i = 0; i < m; ++i) {
    const NUM_REAL *e = NUM_DATA(edges) + 4 * i;
    if (e[0] < 1 || e[0] > n || NUM_MATH(floor)(e[0]) != e[0] || e[1] < 1 ||
        e[1] > n || NUM_MATH(floor)(e[1]) != e[1] || e[0] == e[1] || e[2] < 0 ||
        e[3] < 0)
      return luaL_error(s, "invalid constraint edge");
  }
  NUM_REAL *q = NUM_DATA(p) + p->count, *l = NUM_DATA(lambda) + lambda->count;
  memcpy(q, NUM_DATA(p), 3 * n * sizeof(NUM_REAL));
  memset(l, 0, m * sizeof(NUM_REAL));
  for (size_t pass = 0; pass < iterations; ++pass)
    for (size_t k = 0; k < m; ++k) {
      size_t i = pass % 2 ? m - 1 - k : k;
      const NUM_REAL *e = NUM_DATA(edges) + 4 * i;
      size_t ia = (size_t)e[0] - 1, ib = (size_t)e[1] - 1;
      NUM_REAL wa = NUM_DATA(w)[ia], wb = NUM_DATA(w)[ib];
      NUM_REAL *a = q + ia * 3, *b = q + ib * 3, x = b[0] - a[0],
               y = b[1] - a[1], z = b[2] - a[2],
               d = NUM_MATH(sqrt)(x * x + y * y + z * z);
      if (d < NUM_C(1e-12) || wa + wb == 0)
        continue;
      NUM_REAL alpha = e[3] / (dt * dt),
               next = l[i] + (-(d - e[2]) - alpha * l[i]) / (wa + wb + alpha);
      if (tension)
        next = NUM_MATH(fmin)(0, next);
      NUM_REAL v = (next - l[i]) / d;
      l[i] = next;
      a[0] -= wa * v * x;
      a[1] -= wa * v * y;
      a[2] -= wa * v * z;
      b[0] += wb * v * x;
      b[1] += wb * v * y;
      b[2] += wb * v * z;
    }
  for (size_t i = 0; i < 3 * n; ++i)
    NUM_NAME(checked)(s, q[i]);
  for (size_t i = 0; i < m; ++i)
    NUM_NAME(checked)(s, l[i]);
  NUM_NAME(commit)(s, p, 3 * n);
  NUM_NAME(commit)(s, lambda, m);
  return 0;
}
/* One sweep retains caller multipliers, allowing constraints between passes. */
static int NUM_NAME(relax_sweep)(lua_State *s) {
  h2_numeric_buffer_t *p = NUM_NAME(buffer_check)(s, 1),
                      *e = NUM_NAME(buffer_check)(s, 2),
                      *w = NUM_NAME(buffer_check)(s, 3),
                      *l = NUM_NAME(buffer_check)(s, 4);
  NUM_REAL dt = NUM_NAME(number)(s, 5);
  size_t n = h2_numeric_size(s, 6, 256), m = h2_numeric_size(s, 7, 512);
  luaL_checktype(s, 8, LUA_TBOOLEAN);
  luaL_checktype(s, 9, LUA_TBOOLEAN);
  int reverse = lua_toboolean(s, 8), tension = lua_toboolean(s, 9);
  /* Optional geometric cutoff, independent of compliance/material policy. */
  NUM_REAL epsilon = lua_isnone(s, 10) ? NUM_C(1e-12) : NUM_NAME(number)(s, 10);
  if (!n || dt < NUM_C(1e-6) || dt > NUM_C(.1) || epsilon < 0 || p == e ||
      p == w || p == l || l == e || l == w)
    return luaL_error(s, "invalid sweep parameters or aliases");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, e, 4 * m);
  h2_numeric_capacity(s, w, 2 * m);
  h2_numeric_capacity(s, l, m);
  NUM_REAL *q = NUM_DATA(p) + p->count, *lambda = NUM_DATA(l) + l->count;
  memcpy(q, NUM_DATA(p), 3 * n * sizeof(NUM_REAL));
  memcpy(lambda, NUM_DATA(l), m * sizeof(NUM_REAL));
  for (size_t k = 0; k < m; ++k) {
    size_t i = reverse ? m - 1 - k : k;
    const NUM_REAL *edge = NUM_DATA(e) + 4 * i;
    if (edge[0] < 1 || edge[0] > n || NUM_MATH(floor)(edge[0]) != edge[0] ||
        edge[1] < 1 || edge[1] > n || NUM_MATH(floor)(edge[1]) != edge[1] ||
        edge[0] == edge[1] || edge[2] < 0 || edge[3] < 0 ||
        NUM_DATA(w)[2 * i] < 0 || NUM_DATA(w)[2 * i + 1] < 0)
      return luaL_error(s, "invalid weighted edge");
    NUM_REAL *a = q + 3 * ((size_t)edge[0] - 1),
             *b = q + 3 * ((size_t)edge[1] - 1);
    NUM_REAL delta[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    NUM_REAL d = NUM_MATH(sqrt)(delta[0] * delta[0] + delta[1] * delta[1] +
                                delta[2] * delta[2]);
    NUM_REAL wa = NUM_DATA(w)[2 * i], wb = NUM_DATA(w)[2 * i + 1];
    if (d == 0 || d < epsilon || wa + wb == 0)
      continue;
    NUM_REAL alpha = edge[3] / (dt * dt), old = lambda[i];
    NUM_REAL next = old + (-(d - edge[2]) - alpha * old) / (wa + wb + alpha);
    if (tension)
      next = NUM_MATH(fmin)(0, next);
    lambda[i] = NUM_NAME(checked)(s, next);
    for (size_t j = 0; j < 3; ++j) {
      NUM_REAL correction = (next - old) / d * delta[j];
      a[j] = NUM_NAME(checked)(s, a[j] - wa * correction);
      b[j] = NUM_NAME(checked)(s, b[j] + wb * correction);
    }
  }
  NUM_NAME(commit)(s, p, 3 * n);
  NUM_NAME(commit)(s, l, m);
  return 0;
}
static int NUM_NAME(damp_edges)(lua_State *s) {
  h2_numeric_buffer_t *p = NUM_NAME(buffer_check)(s, 1),
                      *prev = NUM_NAME(buffer_check)(s, 2),
                      *e = NUM_NAME(buffer_check)(s, 3),
                      *w = NUM_NAME(buffer_check)(s, 4);
  NUM_REAL blend = NUM_NAME(number)(s, 5), threshold = NUM_NAME(number)(s, 6),
           epsilon = NUM_NAME(number)(s, 7);
  size_t n = h2_numeric_size(s, 8, 256), m = h2_numeric_size(s, 9, 512);
  if (!n || prev == p || prev == e || prev == w || blend < 0 || blend > 1 ||
      threshold < 0 || epsilon < 0)
    return luaL_error(s, "invalid edge damping parameters or aliases");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, e, 4 * m);
  h2_numeric_capacity(s, w, 2 * m);
  NUM_REAL *q = NUM_DATA(prev) + prev->count;
  memcpy(q, NUM_DATA(prev), 3 * n * sizeof(NUM_REAL));
  for (size_t i = 0; i < m; ++i) {
    const NUM_REAL *edge = NUM_DATA(e) + 4 * i;
    if (edge[0] < 1 || edge[0] > n || NUM_MATH(floor)(edge[0]) != edge[0] ||
        edge[1] < 1 || edge[1] > n || NUM_MATH(floor)(edge[1]) != edge[1] ||
        edge[0] == edge[1] || edge[2] < 0 || edge[3] < 0 ||
        NUM_DATA(w)[2 * i] < 0 || NUM_DATA(w)[2 * i + 1] < 0)
      return luaL_error(s, "invalid weighted edge");
    size_t ia = 3 * ((size_t)edge[0] - 1), ib = 3 * ((size_t)edge[1] - 1);
    NUM_REAL delta[3], squared = 0, axial = 0;
    for (size_t j = 0; j < 3; ++j) {
      delta[j] = NUM_DATA(p)[ib + j] - NUM_DATA(p)[ia + j];
      squared += delta[j] * delta[j];
      axial +=
          (NUM_DATA(p)[ib + j] - q[ib + j] - NUM_DATA(p)[ia + j] + q[ia + j]) *
          delta[j];
    }
    NUM_REAL d = NUM_MATH(sqrt)(squared), wa = NUM_DATA(w)[2 * i],
             wb = NUM_DATA(w)[2 * i + 1];
    if (d <= epsilon || d < edge[2] * threshold || axial <= 0 || wa + wb == 0)
      continue;
    NUM_REAL impulse = axial * blend / (wa + wb) / squared;
    for (size_t j = 0; j < 3; ++j) {
      q[ia + j] = NUM_NAME(checked)(s, q[ia + j] - wa * impulse * delta[j]);
      q[ib + j] = NUM_NAME(checked)(s, q[ib + j] + wb * impulse * delta[j]);
    }
  }
  NUM_NAME(commit)(s, prev, 3 * n);
  return 0;
}
static int NUM_NAME(map)(lua_State *s) {
  static const char *const names[] = {"abs", "sqrt",  "sin",
                                      "cos", "floor", NULL};
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *a = NUM_NAME(buffer_check)(s, 2);
  int op = luaL_checkoption(s, 3, NULL, names);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  for (size_t i = 0; i < n; ++i) {
    NUM_REAL x = NUM_DATA(a)[i], v = 0;
    switch (op) {
    case 0:
      v = NUM_MATH(fabs)(x);
      break;
    case 1:
      v = NUM_MATH(sqrt)(x);
      break;
    case 2:
      v = NUM_MATH(sin)(x);
      break;
    case 3:
      v = NUM_MATH(cos)(x);
      break;
    case 4:
      v = NUM_MATH(floor)(x);
      break;
    }
    NUM_DATA(d)[d->count + i] = v;
  }
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(select_le)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *test = NUM_NAME(buffer_check)(s, 2),
                      *yes = NUM_NAME(buffer_check)(s, 4),
                      *no = NUM_NAME(buffer_check)(s, 5);
  NUM_REAL threshold = NUM_NAME(number)(s, 3);
  size_t n = h2_numeric_size(s, 6, d->count);
  h2_numeric_capacity(s, test, n);
  h2_numeric_capacity(s, yes, n);
  h2_numeric_capacity(s, no, n);
  for (size_t i = 0; i < n; ++i)
    NUM_DATA(d)
    [d->count + i] =
        NUM_DATA(test)[i] <= threshold ? NUM_DATA(yes)[i] : NUM_DATA(no)[i];
  NUM_NAME(commit)(s, d, n);
  return 0;
}
static int NUM_NAME(take)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *src = NUM_NAME(buffer_check)(s, 2),
                      *ids = NUM_NAME(buffer_check)(s, 3);
  size_t width = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT),
         n = h2_numeric_size(s, 5, ids->count);
  if (!width || n > d->count / width)
    return luaL_error(s, "invalid row extent");
  for (size_t i = 0; i < n; ++i) {
    NUM_REAL id = NUM_DATA(ids)[i];
    if (id < 1 || id > src->count / width || NUM_MATH(floor)(id) != id)
      return luaL_error(s, "invalid row index");
    memcpy(NUM_DATA(d) + d->count + i * width,
           NUM_DATA(src) + ((size_t)id - 1) * width, width * sizeof(NUM_REAL));
  }
  NUM_NAME(commit)(s, d, n * width);
  return 0;
}
static int NUM_NAME(damp)(lua_State *s) {
  h2_numeric_buffer_t *p = NUM_NAME(buffer_check)(s, 1),
                      *prev = NUM_NAME(buffer_check)(s, 2),
                      *w = NUM_NAME(buffer_check)(s, 3);
  NUM_REAL retain = NUM_NAME(number)(s, 4), blend = NUM_NAME(number)(s, 5);
  size_t n = h2_numeric_size(s, 6, 256);
  if (p == prev || prev == w || retain < 0 || retain > 1 || blend < 0 ||
      blend > 1)
    return luaL_error(s, "invalid damping parameters");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, w, n);
  for (size_t i = 0; i < n; ++i) {
    if (NUM_DATA(w)[i] < 0)
      return luaL_error(s, "negative inverse mass");
    for (size_t j = 0; j < 3; ++j) {
      size_t k = 3 * i + j;
      NUM_REAL v = NUM_DATA(p)[k] - NUM_DATA(prev)[k], mean = v;
      if (i > 0 && i + 1 < n)
        mean = ((NUM_DATA(p)[k - 3] - NUM_DATA(prev)[k - 3]) +
                (NUM_DATA(p)[k + 3] - NUM_DATA(prev)[k + 3])) *
               NUM_C(.5);
      NUM_DATA(prev)
      [prev->count + k] =
          NUM_DATA(w)[i] == 0
              ? NUM_DATA(prev)[k]
              : NUM_DATA(p)[k] - (v + (mean - v) * blend) * retain;
    }
  }
  NUM_NAME(commit)(s, prev, 3 * n);
  return 0;
}
