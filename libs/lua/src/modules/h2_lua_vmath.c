#include "h2_lua_numeric_internal.h"

static double checked(lua_State *s, double v) {
  if (!isfinite(v) || fabs(v) > H2_LUA_NUMERIC_VALUE_LIMIT)
    luaL_error(s, "numeric value outside finite bounds");
  return v;
}
double h2_numeric_number(lua_State *s, int at) {
  luaL_checktype(s, at, LUA_TNUMBER);
  return checked(s, lua_tonumber(s, at));
}
size_t h2_numeric_size(lua_State *s, int at, size_t max) {
  luaL_checktype(s, at, LUA_TNUMBER);
  lua_Integer n = luaL_checkinteger(s, at);
  if (n < 0 || (lua_Unsigned)n > max)
    luaL_error(s, "numeric size out of bounds");
  return (size_t)n;
}
h2_numeric_buffer_t *h2_numeric_check(lua_State *s, int at) {
  return luaL_checkudata(s, at, H2_NUMERIC_META);
}
void h2_numeric_capacity(lua_State *s, h2_numeric_buffer_t *b, size_t n) {
  if (n > b->count)
    luaL_error(s, "numeric buffer too small");
}
void h2_numeric_commit(lua_State *s, h2_numeric_buffer_t *b, size_t n) {
  double *work = b->data + b->count;
  for (size_t i = 0; i < n; ++i)
    checked(s, work[i]);
  memcpy(b->data, work, n * sizeof(double));
}
static int buffer_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 1, H2_LUA_NUMERIC_COUNT_LIMIT);
  h2_numeric_buffer_t *b =
      lua_newuserdatauv(s, sizeof(*b) + 2 * n * sizeof(double), 0);
  b->count = n;
  memset(b->data, 0, 2 * n * sizeof(double));
  luaL_setmetatable(s, H2_NUMERIC_META);
  return 1;
}
static int buffer_len(lua_State *s) {
  lua_pushinteger(s, (lua_Integer)h2_numeric_check(s, 1)->count);
  return 1;
}
static size_t index_of(lua_State *s, int at, size_t n) {
  size_t i = h2_numeric_size(s, at, n);
  if (!i)
    luaL_error(s, "indices are one-based");
  return i - 1;
}
static int buffer_get(lua_State *s) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, 1);
  lua_pushnumber(s, b->data[index_of(s, 2, b->count)]);
  return 1;
}
static int buffer_set(lua_State *s) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, 1);
  size_t i = index_of(s, 2, b->count);
  double v = h2_numeric_number(s, 3);
  b->data[i] = v;
  return 0;
}
static int buffer_fill(lua_State *s) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, 1);
  double v = h2_numeric_number(s, 2);
  for (size_t i = 0; i < b->count; ++i)
    b->data[i] = v;
  return 0;
}
static int buffer_load(lua_State *s) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, 1);
  luaL_checktype(s, 2, LUA_TTABLE);
  size_t n = lua_rawlen(s, 2);
  h2_numeric_capacity(s, b, n);
  for (size_t i = 0; i < n; ++i) {
    lua_rawgeti(s, 2, (lua_Integer)i + 1);
    b->data[b->count + i] = h2_numeric_number(s, -1);
    lua_pop(s, 1);
  }
  h2_numeric_commit(s, b, n);
  return 0;
}
static int buffer_copy(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *b = h2_numeric_check(s, 2);
  size_t di = index_of(s, 3, d->count), bi = index_of(s, 4, b->count);
  size_t n = h2_numeric_size(s, 5, b->count - bi);
  h2_numeric_capacity(s, d, di + n);
  memmove(d->data + di, b->data + bi, n * sizeof(double));
  return 0;
}
/* Strided channel transfer lets Lua compose bulk formulas on interleaved
 * coordinates without crossing the native boundary for every vertex. */
static int channel(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *src = h2_numeric_check(s, 2);
  int scatter = (int)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_buffer_t *strided = scatter ? d : src, *packed = scatter ? src : d;
  size_t first = index_of(s, 3, strided->count),
         stride = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT);
  size_t n = h2_numeric_size(s, 5, packed->count);
  if (!stride || (n && (n - 1) > (strided->count - 1 - first) / stride))
    return luaL_error(s, "strided range out of bounds");
  if (scatter) {
    memcpy(d->data + d->count, d->data, d->count * sizeof(double));
    for (size_t i = 0; i < n; ++i)
      d->data[d->count + first + i * stride] = src->data[i];
    h2_numeric_commit(s, d, d->count);
  } else {
    for (size_t i = 0; i < n; ++i)
      d->data[d->count + i] = src->data[first + i * stride];
    h2_numeric_commit(s, d, n);
  }
  return 0;
}
static int scalar_clamp(lua_State *s) {
  double x = h2_numeric_number(s, 1), a = h2_numeric_number(s, 2),
         b = h2_numeric_number(s, 3);
  if (a > b)
    return luaL_error(s, "reversed interval");
  lua_pushnumber(s, fmax(a, fmin(b, x)));
  return 1;
}
static int scalar_lerp(lua_State *s) {
  double a = h2_numeric_number(s, 1), b = h2_numeric_number(s, 2),
         t = h2_numeric_number(s, 3);
  lua_pushnumber(s, checked(s, a + (b - a) * t));
  return 1;
}
static int scalar_smoothstep(lua_State *s) {
  double a = h2_numeric_number(s, 1), b = h2_numeric_number(s, 2),
         x = h2_numeric_number(s, 3);
  if (a >= b)
    return luaL_error(s, "empty or reversed interval");
  double t = fmax(0, fmin(1, (x - a) / (b - a)));
  lua_pushnumber(s, t * t * (3 - 2 * t));
  return 1;
}
static int spring(lua_State *s) {
  double x = h2_numeric_number(s, 1), v = h2_numeric_number(s, 2),
         target = h2_numeric_number(s, 3);
  double k = h2_numeric_number(s, 4), c = h2_numeric_number(s, 5),
         a = h2_numeric_number(s, 6), dt = h2_numeric_number(s, 7);
  if (k < 0 || c < 0 || dt < 1e-6 || dt > .1)
    return luaL_error(s, "invalid spring parameters");
  v = checked(s, v + (k * (target - x) - c * v + a) * dt);
  x = checked(s, x + v * dt);
  lua_pushnumber(s, x);
  lua_pushnumber(s, v);
  return 2;
}
/* All bulk operations read public values and publish from VM-owned scratch.
 * This permits aliasing and preserves every output on numeric overflow. */
static int combine(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *a = h2_numeric_check(s, 2),
                      *b = h2_numeric_check(s, 3);
  double ka = h2_numeric_number(s, 4), kb = h2_numeric_number(s, 5),
         bias = h2_numeric_number(s, 6);
  size_t n = h2_numeric_size(s, 7, d->count);
  h2_numeric_capacity(s, a, n);
  h2_numeric_capacity(s, b, n);
  for (size_t i = 0; i < n; ++i)
    d->data[d->count + i] = ka * a->data[i] + kb * b->data[i] + bias;
  h2_numeric_commit(s, d, n);
  return 0;
}
static int polynomial(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *a = h2_numeric_check(s, 2),
                      *c = h2_numeric_check(s, 3);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  if (!c->count || c->count > 9)
    return luaL_error(s, "polynomial requires 1..9 coefficients");
  for (size_t i = 0; i < n; ++i) {
    double v = 0;
    for (size_t j = c->count; j > 0; --j)
      v = v * a->data[i] + c->data[j - 1];
    d->data[d->count + i] = v;
  }
  h2_numeric_commit(s, d, n);
  return 0;
}
static int clamp_bulk(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *a = h2_numeric_check(s, 2);
  double lo = h2_numeric_number(s, 3), hi = h2_numeric_number(s, 4);
  size_t n = h2_numeric_size(s, 5, d->count);
  h2_numeric_capacity(s, a, n);
  if (lo > hi)
    return luaL_error(s, "reversed interval");
  for (size_t i = 0; i < n; ++i)
    d->data[d->count + i] = fmax(lo, fmin(hi, a->data[i]));
  h2_numeric_commit(s, d, n);
  return 0;
}
static int product(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *a = h2_numeric_check(s, 2),
                      *b = h2_numeric_check(s, 3);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  h2_numeric_capacity(s, b, n);
  int divide = (int)lua_tointeger(s, lua_upvalueindex(1));
  for (size_t i = 0; i < n; ++i) {
    if (divide && b->data[i] == 0)
      return luaL_error(s, "division by zero");
    d->data[d->count + i] =
        divide ? a->data[i] / b->data[i] : a->data[i] * b->data[i];
  }
  h2_numeric_commit(s, d, n);
  return 0;
}
static int vector_length(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2);
  size_t n = h2_numeric_size(s, 3, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  int normalize = (int)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, d, normalize ? 3 * n : n);
  for (size_t i = 0; i < n; ++i) {
    const double *v = p->data + 3 * i;
    /* Bounded inputs cannot overflow the squared norm. Scale tiny vectors
     * before normalization so even subnormal components produce unit vectors.
     */
    double squared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    double scale = 1, unit[3] = {v[0], v[1], v[2]};
    if (squared < 1e-280) {
      scale = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));
      if (scale > 0) {
        for (size_t j = 0; j < 3; ++j)
          unit[j] /= scale;
      }
      squared = unit[0] * unit[0] + unit[1] * unit[1] + unit[2] * unit[2];
    }
    double len = sqrt(squared);
    if (normalize) {
      for (size_t j = 0; j < 3; ++j)
        d->data[d->count + 3 * i + j] = len == 0 ? 0 : unit[j] / len;
    } else
      d->data[d->count + i] = scale * len;
  }
  h2_numeric_commit(s, d, normalize ? 3 * n : n);
  return 0;
}
static int dot(lua_State *s) {
  h2_numeric_buffer_t *a = h2_numeric_check(s, 1), *b = h2_numeric_check(s, 2);
  size_t n = h2_numeric_size(s, 3, a->count);
  h2_numeric_capacity(s, b, n);
  double v = 0;
  for (size_t i = 0; i < n; ++i)
    v += a->data[i] * b->data[i];
  lua_pushnumber(s, checked(s, v));
  return 1;
}
static int verlet(lua_State *s) {
  h2_numeric_buffer_t *p = h2_numeric_check(s, 1),
                      *prev = h2_numeric_check(s, 2),
                      *acc = h2_numeric_check(s, 3),
                      *weights = h2_numeric_check(s, 4);
  double dt = h2_numeric_number(s, 5), drag = h2_numeric_number(s, 6);
  size_t n = h2_numeric_size(s, 7, 256);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, acc, 3 * n);
  h2_numeric_capacity(s, weights, n);
  if (p == prev || p == acc || p == weights || prev == acc || prev == weights ||
      dt < 1e-6 || dt > .1 || drag < 0)
    return luaL_error(s, "invalid Verlet buffers or parameters");
  double factor = 1 / (1 + drag * dt);
  for (size_t i = 0; i < n; ++i) {
    if (weights->data[i] < 0)
      return luaL_error(s, "negative inverse mass");
    for (size_t j = 3 * i; j < 3 * i + 3; ++j) {
      p->data[p->count + j] = weights->data[i] == 0
                                  ? p->data[j]
                                  : p->data[j] +
                                        (p->data[j] - prev->data[j]) * factor +
                                        acc->data[j] * dt * dt;
      checked(s, p->data[p->count + j]);
    }
  }
  for (size_t i = 0; i < n; ++i)
    if (weights->data[i] > 0)
      memcpy(prev->data + 3 * i, p->data + 3 * i, 3 * sizeof(double));
  h2_numeric_commit(s, p, 3 * n);
  return 0;
}
static int relax(lua_State *s) {
  h2_numeric_buffer_t *p = h2_numeric_check(s, 1), *w = h2_numeric_check(s, 2),
                      *edges = h2_numeric_check(s, 3),
                      *lambda = h2_numeric_check(s, 4);
  double dt = h2_numeric_number(s, 5);
  size_t iterations = h2_numeric_size(s, 6, 32), n = h2_numeric_size(s, 7, 256),
         m = h2_numeric_size(s, 8, 512);
  luaL_checktype(s, 9, LUA_TBOOLEAN);
  int tension = lua_toboolean(s, 9);
  if (!iterations || !n || dt < 1e-6 || dt > .1 || p == w || p == edges ||
      p == lambda || lambda == w || lambda == edges)
    return luaL_error(s, "invalid constraint parameters or aliases");
  h2_numeric_capacity(s, p, n * 3);
  h2_numeric_capacity(s, w, n);
  h2_numeric_capacity(s, edges, m * 4);
  h2_numeric_capacity(s, lambda, m);
  for (size_t i = 0; i < n; ++i)
    if (w->data[i] < 0)
      return luaL_error(s, "negative inverse mass");
  for (size_t i = 0; i < m; ++i) {
    const double *e = edges->data + 4 * i;
    if (e[0] < 1 || e[0] > n || floor(e[0]) != e[0] || e[1] < 1 || e[1] > n ||
        floor(e[1]) != e[1] || e[0] == e[1] || e[2] < 0 || e[3] < 0)
      return luaL_error(s, "invalid constraint edge");
  }
  double *q = p->data + p->count, *l = lambda->data + lambda->count;
  memcpy(q, p->data, 3 * n * sizeof(double));
  memset(l, 0, m * sizeof(double));
  for (size_t pass = 0; pass < iterations; ++pass)
    for (size_t k = 0; k < m; ++k) {
      size_t i = pass % 2 ? m - 1 - k : k;
      const double *e = edges->data + 4 * i;
      size_t ia = (size_t)e[0] - 1, ib = (size_t)e[1] - 1;
      double wa = w->data[ia], wb = w->data[ib];
      double *a = q + ia * 3, *b = q + ib * 3, x = b[0] - a[0], y = b[1] - a[1],
             z = b[2] - a[2], d = sqrt(x * x + y * y + z * z);
      if (d < 1e-12 || wa + wb == 0)
        continue;
      double alpha = e[3] / (dt * dt),
             next = l[i] + (-(d - e[2]) - alpha * l[i]) / (wa + wb + alpha);
      if (tension)
        next = fmin(0, next);
      double v = (next - l[i]) / d;
      l[i] = next;
      a[0] -= wa * v * x;
      a[1] -= wa * v * y;
      a[2] -= wa * v * z;
      b[0] += wb * v * x;
      b[1] += wb * v * y;
      b[2] += wb * v * z;
    }
  for (size_t i = 0; i < 3 * n; ++i)
    checked(s, q[i]);
  for (size_t i = 0; i < m; ++i)
    checked(s, l[i]);
  h2_numeric_commit(s, p, 3 * n);
  h2_numeric_commit(s, lambda, m);
  return 0;
}
/* One sweep retains caller multipliers, allowing constraints between passes. */
static int relax_sweep(lua_State *s) {
  h2_numeric_buffer_t *p = h2_numeric_check(s, 1), *e = h2_numeric_check(s, 2),
                      *w = h2_numeric_check(s, 3), *l = h2_numeric_check(s, 4);
  double dt = h2_numeric_number(s, 5);
  size_t n = h2_numeric_size(s, 6, 256), m = h2_numeric_size(s, 7, 512);
  luaL_checktype(s, 8, LUA_TBOOLEAN);
  luaL_checktype(s, 9, LUA_TBOOLEAN);
  int reverse = lua_toboolean(s, 8), tension = lua_toboolean(s, 9);
  /* Optional geometric cutoff, independent of compliance/material policy. */
  double epsilon = lua_isnone(s, 10) ? 1e-12 : h2_numeric_number(s, 10);
  if (!n || dt < 1e-6 || dt > .1 || epsilon < 0 || p == e || p == w || p == l ||
      l == e || l == w)
    return luaL_error(s, "invalid sweep parameters or aliases");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, e, 4 * m);
  h2_numeric_capacity(s, w, 2 * m);
  h2_numeric_capacity(s, l, m);
  double *q = p->data + p->count, *lambda = l->data + l->count;
  memcpy(q, p->data, 3 * n * sizeof(double));
  memcpy(lambda, l->data, m * sizeof(double));
  for (size_t k = 0; k < m; ++k) {
    size_t i = reverse ? m - 1 - k : k;
    const double *edge = e->data + 4 * i;
    if (edge[0] < 1 || edge[0] > n || floor(edge[0]) != edge[0] ||
        edge[1] < 1 || edge[1] > n || floor(edge[1]) != edge[1] ||
        edge[0] == edge[1] || edge[2] < 0 || edge[3] < 0 ||
        w->data[2 * i] < 0 || w->data[2 * i + 1] < 0)
      return luaL_error(s, "invalid weighted edge");
    double *a = q + 3 * ((size_t)edge[0] - 1),
           *b = q + 3 * ((size_t)edge[1] - 1);
    double delta[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double d =
        sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    double wa = w->data[2 * i], wb = w->data[2 * i + 1];
    if (d == 0 || d < epsilon || wa + wb == 0)
      continue;
    double alpha = edge[3] / (dt * dt), old = lambda[i];
    double next = old + (-(d - edge[2]) - alpha * old) / (wa + wb + alpha);
    if (tension)
      next = fmin(0, next);
    lambda[i] = checked(s, next);
    for (size_t j = 0; j < 3; ++j) {
      double correction = (next - old) / d * delta[j];
      a[j] = checked(s, a[j] - wa * correction);
      b[j] = checked(s, b[j] + wb * correction);
    }
  }
  h2_numeric_commit(s, p, 3 * n);
  h2_numeric_commit(s, l, m);
  return 0;
}
static int damp_edges(lua_State *s) {
  h2_numeric_buffer_t *p = h2_numeric_check(s, 1),
                      *prev = h2_numeric_check(s, 2),
                      *e = h2_numeric_check(s, 3), *w = h2_numeric_check(s, 4);
  double blend = h2_numeric_number(s, 5), threshold = h2_numeric_number(s, 6),
         epsilon = h2_numeric_number(s, 7);
  size_t n = h2_numeric_size(s, 8, 256), m = h2_numeric_size(s, 9, 512);
  if (!n || prev == p || prev == e || prev == w || blend < 0 || blend > 1 ||
      threshold < 0 || epsilon < 0)
    return luaL_error(s, "invalid edge damping parameters or aliases");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, e, 4 * m);
  h2_numeric_capacity(s, w, 2 * m);
  double *q = prev->data + prev->count;
  memcpy(q, prev->data, 3 * n * sizeof(double));
  for (size_t i = 0; i < m; ++i) {
    const double *edge = e->data + 4 * i;
    if (edge[0] < 1 || edge[0] > n || floor(edge[0]) != edge[0] ||
        edge[1] < 1 || edge[1] > n || floor(edge[1]) != edge[1] ||
        edge[0] == edge[1] || edge[2] < 0 || edge[3] < 0 ||
        w->data[2 * i] < 0 || w->data[2 * i + 1] < 0)
      return luaL_error(s, "invalid weighted edge");
    size_t ia = 3 * ((size_t)edge[0] - 1), ib = 3 * ((size_t)edge[1] - 1);
    double delta[3], squared = 0, axial = 0;
    for (size_t j = 0; j < 3; ++j) {
      delta[j] = p->data[ib + j] - p->data[ia + j];
      squared += delta[j] * delta[j];
      axial += (p->data[ib + j] - q[ib + j] - p->data[ia + j] + q[ia + j]) *
               delta[j];
    }
    double d = sqrt(squared), wa = w->data[2 * i], wb = w->data[2 * i + 1];
    if (d <= epsilon || d < edge[2] * threshold || axial <= 0 || wa + wb == 0)
      continue;
    double impulse = axial * blend / (wa + wb) / squared;
    for (size_t j = 0; j < 3; ++j) {
      q[ia + j] = checked(s, q[ia + j] - wa * impulse * delta[j]);
      q[ib + j] = checked(s, q[ib + j] + wb * impulse * delta[j]);
    }
  }
  h2_numeric_commit(s, prev, 3 * n);
  return 0;
}
static int map(lua_State *s) {
  static const char *const names[] = {"abs", "sqrt",  "sin",
                                      "cos", "floor", NULL};
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *a = h2_numeric_check(s, 2);
  int op = luaL_checkoption(s, 3, NULL, names);
  size_t n = h2_numeric_size(s, 4, d->count);
  h2_numeric_capacity(s, a, n);
  for (size_t i = 0; i < n; ++i) {
    double x = a->data[i], v = 0;
    switch (op) {
    case 0:
      v = fabs(x);
      break;
    case 1:
      v = sqrt(x);
      break;
    case 2:
      v = sin(x);
      break;
    case 3:
      v = cos(x);
      break;
    case 4:
      v = floor(x);
      break;
    }
    d->data[d->count + i] = v;
  }
  h2_numeric_commit(s, d, n);
  return 0;
}
static int select_le(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *test = h2_numeric_check(s, 2),
                      *yes = h2_numeric_check(s, 4),
                      *no = h2_numeric_check(s, 5);
  double threshold = h2_numeric_number(s, 3);
  size_t n = h2_numeric_size(s, 6, d->count);
  h2_numeric_capacity(s, test, n);
  h2_numeric_capacity(s, yes, n);
  h2_numeric_capacity(s, no, n);
  for (size_t i = 0; i < n; ++i)
    d->data[d->count + i] =
        test->data[i] <= threshold ? yes->data[i] : no->data[i];
  h2_numeric_commit(s, d, n);
  return 0;
}
static int take(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *src = h2_numeric_check(s, 2),
                      *ids = h2_numeric_check(s, 3);
  size_t width = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT),
         n = h2_numeric_size(s, 5, ids->count);
  if (!width || n > d->count / width)
    return luaL_error(s, "invalid row extent");
  for (size_t i = 0; i < n; ++i) {
    double id = ids->data[i];
    if (id < 1 || id > src->count / width || floor(id) != id)
      return luaL_error(s, "invalid row index");
    memcpy(d->data + d->count + i * width, src->data + ((size_t)id - 1) * width,
           width * sizeof(double));
  }
  h2_numeric_commit(s, d, n * width);
  return 0;
}
static int damp(lua_State *s) {
  h2_numeric_buffer_t *p = h2_numeric_check(s, 1),
                      *prev = h2_numeric_check(s, 2),
                      *w = h2_numeric_check(s, 3);
  double retain = h2_numeric_number(s, 4), blend = h2_numeric_number(s, 5);
  size_t n = h2_numeric_size(s, 6, 256);
  if (p == prev || prev == w || retain < 0 || retain > 1 || blend < 0 ||
      blend > 1)
    return luaL_error(s, "invalid damping parameters");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, prev, 3 * n);
  h2_numeric_capacity(s, w, n);
  for (size_t i = 0; i < n; ++i) {
    if (w->data[i] < 0)
      return luaL_error(s, "negative inverse mass");
    for (size_t j = 0; j < 3; ++j) {
      size_t k = 3 * i + j;
      double v = p->data[k] - prev->data[k], mean = v;
      if (i > 0 && i + 1 < n)
        mean = ((p->data[k - 3] - prev->data[k - 3]) +
                (p->data[k + 3] - prev->data[k + 3])) *
               .5;
      prev->data[prev->count + k] =
          w->data[i] == 0 ? prev->data[k]
                          : p->data[k] - (v + (mean - v) * blend) * retain;
    }
  }
  h2_numeric_commit(s, prev, 3 * n);
  return 0;
}
int h2_lua_open_vmath(lua_State *s) {
  if (luaL_newmetatable(s, H2_NUMERIC_META)) {
    static const luaL_Reg methods[] = {
        {"get", buffer_get},   {"set", buffer_set},   {"fill", buffer_fill},
        {"load", buffer_load}, {"copy", buffer_copy}, {"__len", buffer_len},
        {NULL, NULL}};
    luaL_setfuncs(s, methods, 0);
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "numeric buffer");
    lua_setfield(s, -2, "__metatable");
  }
  lua_pop(s, 1);
  static const luaL_Reg functions[] = {{"buffer", buffer_new},
                                       {"clamp", scalar_clamp},
                                       {"lerp", scalar_lerp},
                                       {"smoothstep", scalar_smoothstep},
                                       {"spring", spring},
                                       {"combine", combine},
                                       {"polynomial", polynomial},
                                       {"clamp_bulk", clamp_bulk},
                                       {"dot", dot},
                                       {"verlet", verlet},
                                       {"relax", relax},
                                       {"damp", damp},
                                       {"relax_sweep", relax_sweep},
                                       {"damp_edges", damp_edges},
                                       {"map", map},
                                       {"select_le", select_le},
                                       {"take", take},
                                       {NULL, NULL}};
  luaL_newlib(s, functions);
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, product, 1);
  lua_setfield(s, -2, "multiply");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, product, 1);
  lua_setfield(s, -2, "divide");
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, vector_length, 1);
  lua_setfield(s, -2, "length3");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, vector_length, 1);
  lua_setfield(s, -2, "normalize3");
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, channel, 1);
  lua_setfield(s, -2, "gather");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, channel, 1);
  lua_setfield(s, -2, "scatter");
  return 1;
}
