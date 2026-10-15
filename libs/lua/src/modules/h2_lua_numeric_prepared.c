#include "h2_lua_numeric_internal.h"
#include "h2_lua_numeric_compensated_internal.h"
#include "h2_f32_math.h"

#define WORK_META "h2.numeric.constraints"
#define NODE_LIMIT 256u
#define EDGE_LIMIT 512u

enum { EDGE_FIXED = 1u, EDGE_MOVE_A = 2u, EDGE_MOVE_B = 4u };
typedef struct prepared_edge {
  size_t a, b;
  double rest, compliance, wa, wb, alpha64;
  float inverse, weight_a, weight_b, alpha, rest_f;
  precise_float rest_squared;
  unsigned int predicates;
} prepared_edge_t;
typedef struct constraint_workspace {
  size_t node_capacity, edge_capacity, n, m, sweep;
  double dt, span_lambda;
  int has_span;
  prepared_edge_t span;
  double *p, *previous, *staged_p, *staged_previous;
  double *owned_p;
  h2_numeric_buffer_t *bound_p, *bound_previous;
  prepared_edge_t *edges, *staged_edges;
  float *lambda, *staged_lambda, *displacement;
  precise_float *coordinates;
} constraint_workspace_t;

static double finite_result(lua_State *s, double value) {
  /* A finite positive limit also rejects NaN/infinity via the ordered check,
   * without separate software-double finiteness comparisons on FPU hosts. */
  if (!(fabs(value) <= H2_LUA_NUMERIC_VALUE_LIMIT))
    luaL_error(s, "prepared numeric result out of bounds");
  return value;
}
static h2_numeric_buffer_t *f64_buffer(lua_State *s, int at, size_t count) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, at);
  if (b->is_f32)
    luaL_error(s, "prepared state and descriptors require f64 buffers");
  h2_numeric_capacity(s, b, count);
  return b;
}
static constraint_workspace_t *workspace(lua_State *s) {
  return luaL_checkudata(s, 1, WORK_META);
}
static void loaded(lua_State *s, constraint_workspace_t *w) {
  if (!w->n)
    luaL_error(s, "constraint workspace is not loaded");
}
static void separate_state(lua_State *s, constraint_workspace_t *w,
                           h2_numeric_buffer_t *b) {
  if (b && (b == w->bound_p || b == w->bound_previous))
    luaL_error(s, "argument aliases bound constraint state");
}
/* Buffer uservalue 1 is a private table with a weak workspace value. It cannot
 * keep a dead workspace alive, and no native pointer is borrowed from Lua. */
static void available_state(lua_State *s, int at) {
  if (lua_getiuservalue(s, at, 1) == LUA_TTABLE) {
    lua_rawgeti(s, -1, 1);
    int occupied = !lua_isnil(s, -1) && !lua_rawequal(s, -1, 1);
    lua_pop(s, 2);
    if (occupied)
      luaL_error(s, "numeric buffer is bound to another workspace");
  } else
    lua_pop(s, 1);
}
static void release_state(lua_State *s, constraint_workspace_t *w) {
  if (!w->bound_p)
    return;
  for (int slot = 5; slot <= 6; ++slot) {
    lua_getiuservalue(s, 1, slot);
    lua_pushnil(s);
    lua_setiuservalue(s, -2, 1);
    lua_pop(s, 1);
    lua_pushnil(s);
    lua_setiuservalue(s, 1, slot);
  }
  w->bound_p = w->bound_previous = NULL;
}
static void validate_state(lua_State *s, constraint_workspace_t *w,
                           int previous) {
  if (!w->bound_p)
    return;
  for (size_t i = 0; i < 3 * w->n; ++i) {
    finite_result(s, w->p[i]);
    if (previous)
      finite_result(s, w->previous[i]);
  }
}
static size_t node_index(lua_State *s, double value, size_t n) {
  if (value < 1 || value > (double)n || floor(value) != value)
    luaL_error(s, "invalid constraint index");
  return (size_t)value - 1;
}
static double step(lua_State *s, int at) {
  double dt = h2_numeric_number(s, at);
  if (dt < 1e-6 || dt > .1)
    luaL_error(s, "invalid constraint timestep");
  return dt;
}
static void prepare(prepared_edge_t *e, double dt) {
  /* Test the original doubles, not weights rounded to float. A positive
   * subnormal weight must still enter the source arithmetic/error path. */
  e->predicates = (e->wa + e->wb == 0 ? EDGE_FIXED : 0) |
                  (e->wa > 0 ? EDGE_MOVE_A : 0) |
                  (e->wb > 0 ? EDGE_MOVE_B : 0);
  double alpha = e->compliance / (dt * dt);
  e->alpha64 = alpha;
  double denominator = e->wa + e->wb + alpha;
  e->inverse = denominator > 0 ? 1.0f / (float)denominator : 0;
  e->weight_a = (float)e->wa;
  e->weight_b = (float)e->wb;
  e->alpha = (float)alpha;
  e->rest_f = (float)e->rest;
  e->rest_squared = pf_from(e->rest * e->rest);
}
static prepared_edge_t edge_read(lua_State *s, const double *row, size_t n,
                                 double dt) {
  prepared_edge_t e = {0};
  for (size_t i = 0; i < 6; ++i)
    finite_result(s, row[i]);
  e.a = node_index(s, row[0], n);
  e.b = node_index(s, row[1], n);
  if (e.a == e.b || row[2] < 0 || row[3] < 0 || row[4] < 0 || row[5] < 0)
    luaL_error(s, "invalid constraint edge");
  e.rest = row[2];
  e.compliance = row[3];
  e.wa = row[4];
  e.wb = row[5];
  prepare(&e, dt);
  return e;
}
static int workspace_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 1, NODE_LIMIT);
  size_t m = h2_numeric_size(s, 2, EDGE_LIMIT);
  if (!n)
    return luaL_error(s, "zero node capacity");
  /* Separate userdata payloads are retained by the workspace, naturally aligned
   * and charged to the VM even when a later allocation fails. */
  constraint_workspace_t *w = lua_newuserdatauv(s, sizeof(*w), 6);
  memset(w, 0, sizeof(*w));
  w->node_capacity = n;
  w->edge_capacity = m;
  luaL_setmetatable(s, WORK_META);
  int at = lua_gettop(s);
  w->p = lua_newuserdatauv(s, 12 * n * sizeof(double), 0);
  w->owned_p = w->p;
  w->previous = w->p + 3 * n;
  w->staged_p = w->previous + 3 * n;
  w->staged_previous = w->staged_p + 3 * n;
  lua_setiuservalue(s, at, 1);
  w->edges = lua_newuserdatauv(s, 2 * m * sizeof(prepared_edge_t), 0);
  w->staged_edges = w->edges + m;
  lua_setiuservalue(s, at, 2);
  w->lambda = lua_newuserdatauv(s, (2 * m + 3 * n) * sizeof(float), 0);
  w->staged_lambda = w->lambda + m;
  w->displacement = w->staged_lambda + m;
  lua_setiuservalue(s, at, 3);
  w->coordinates = lua_newuserdatauv(s, 3 * n * sizeof(precise_float), 0);
  lua_setiuservalue(s, at, 4);
  return 1;
}
static int workspace_load(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  int bind = lua_toboolean(s, lua_upvalueindex(1));
  int owner = 0;
  if (bind) {
    /* Finish every allocation before inspecting mutable inputs or staging
     * metadata: a GC finalizer may reenter during these allocations. */
    lua_createtable(s, 1, 0);
    owner = lua_gettop(s);
    lua_createtable(s, 0, 1);
    lua_pushliteral(s, "v");
    lua_setfield(s, -2, "__mode");
    lua_setmetatable(s, owner);
    lua_pushvalue(s, 1);
    lua_rawseti(s, owner, 1);
  }
  size_t n = h2_numeric_size(s, 5, w->node_capacity);
  size_t m = h2_numeric_size(s, 6, w->edge_capacity);
  if (!n)
    return luaL_error(s, "zero active nodes");
  double dt = step(s, 7);
  h2_numeric_buffer_t *p = f64_buffer(s, 2, 3 * n),
                      *prev = f64_buffer(s, 3, 3 * n),
                      *e = f64_buffer(s, 4, 6 * m);
  if (p == prev || p == e || prev == e)
    return luaL_error(s, "aliased constraint buffers");
  if (bind) {
    available_state(s, 2);
    available_state(s, 3);
  }
  for (size_t i = 0; i < 3 * n; ++i) {
    finite_result(s, p->data.f64[i]);
    finite_result(s, prev->data.f64[i]);
  }
  for (size_t i = 0; i < m; ++i)
    w->staged_edges[i] = edge_read(s, e->data.f64 + 6 * i, n, dt);
  /* No allocation or Lua callback from here through the complete publication. */
  if (!bind) {
    memcpy(w->owned_p, p->data.f64, 3 * n * sizeof(double));
    memcpy(w->owned_p + 3 * w->node_capacity, prev->data.f64,
           3 * n * sizeof(double));
  }
  release_state(s, w);
  if (bind) {
    for (int arg = 2; arg <= 3; ++arg) {
      lua_pushvalue(s, owner);
      lua_setiuservalue(s, arg, 1);
      lua_pushvalue(s, arg);
      lua_setiuservalue(s, 1, arg + 3);
    }
    w->bound_p = p;
    w->bound_previous = prev;
    w->p = p->data.f64;
    w->previous = prev->data.f64;
  } else {
    w->p = w->owned_p;
    w->previous = w->owned_p + 3 * w->node_capacity;
  }
  memcpy(w->edges, w->staged_edges, m * sizeof(prepared_edge_t));
  memset(w->lambda, 0, m * sizeof(float));
  w->n = n;
  w->m = m;
  w->dt = dt;
  w->sweep = 0;
  w->span_lambda = 0;
  w->has_span = 0;
  return 0;
}
static int workspace_begin(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  double dt = step(s, 2);
  if (dt != w->dt) {
    for (size_t i = 0; i < w->m; ++i)
      prepare(w->edges + i, dt);
    if (w->has_span)
      prepare(&w->span, dt);
  }
  w->dt = dt;
  w->sweep = 0;
  w->span_lambda = 0;
  memset(w->lambda, 0, w->m * sizeof(float));
  return 0;
}
static int workspace_node(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  size_t i = node_index(s, h2_numeric_number(s, 2), w->n);
  if (lua_gettop(s) == 2) {
    for (size_t j = 0; j < 3; ++j)
      lua_pushnumber(s, w->p[3 * i + j]);
    for (size_t j = 0; j < 3; ++j)
      lua_pushnumber(s, w->previous[3 * i + j]);
    return 6;
  }
  double values[6];
  for (int j = 0; j < 6; ++j)
    values[j] = h2_numeric_number(s, 3 + j);
  memcpy(w->p + 3 * i, values, 3 * sizeof(double));
  memcpy(w->previous + 3 * i, values + 3, 3 * sizeof(double));
  return 0;
}
static int workspace_edge(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  size_t i = node_index(s, h2_numeric_number(s, 2), w->m);
  double row[6];
  for (int j = 0; j < 6; ++j)
    row[j] = h2_numeric_number(s, 3 + j);
  prepared_edge_t e = edge_read(s, row, w->n, w->dt);
  w->edges[i] = e;
  return 0;
}
static int workspace_span(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  if (lua_isnil(s, 2)) {
    w->has_span = 0;
    return 0;
  }
  double row[6];
  for (int j = 0; j < 6; ++j)
    row[j] = h2_numeric_number(s, 2 + j);
  prepared_edge_t e = edge_read(s, row, w->n, w->dt);
  w->span = e;
  w->has_span = 1;
  return 0;
}
/* Bounds rows: first, count, axis, lower, upper. Decode before any phase write.
 */
static const double *bounds_read(lua_State *s, int at, size_t count, size_t n) {
  if (lua_isnoneornil(s, at)) {
    if (count)
      luaL_error(s, "missing bounds");
    return NULL;
  }
  const double *b = f64_buffer(s, at, 5 * count)->data.f64;
  for (size_t k = 0; k < count; ++k) {
    const double *r = b + 5 * k;
    for (size_t j = 0; j < 5; ++j)
      finite_result(s, r[j]);
    size_t first = node_index(s, r[0], n);
    if (r[1] < 0 || r[1] > (double)(n - first) || floor(r[1]) != r[1] ||
        r[2] < 1 || r[2] > 3 || floor(r[2]) != r[2] || r[3] > r[4])
      luaL_error(s, "invalid positional bounds");
  }
  return b;
}
/* Keep the source norm in the same no-fast-math compilation unit as its
 * refined seed. This explicit API does not alter ordinary length/normalize. */
static int length3_refined(lua_State *s) {
  size_t n = h2_numeric_size(s, 3, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  h2_numeric_buffer_t *out = f64_buffer(s, 1, n),
                      *src = f64_buffer(s, 2, 3 * n);
  double *staged = out->data.f64 + out->count;
  for (size_t i = 0; i < n; ++i) {
    const double *v = src->data.f64 + 3 * i;
    double x = finite_result(s, v[0]), y = finite_result(s, v[1]),
           z = finite_result(s, v[2]);
    staged[i] = finite_result(s, refined_sqrt(x * x + y * y + z * z));
  }
  memcpy(out->data.f64, staged, n * sizeof(double));
  return 0;
}
static int workspace_displacements(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  h2_numeric_buffer_t *out = h2_numeric_check(s, 2);
  if (!out->is_f32)
    return luaL_error(s, "displacements require an f32 output buffer");
  size_t first = node_index(s, h2_numeric_number(s, 3), w->n);
  size_t count = h2_numeric_size(s, 4, w->n - first);
  h2_numeric_capacity(s, out, 3 * count);
  float *staged = out->data.f32 + out->count;
  for (size_t i = 0; i < 3 * count; ++i) {
    size_t at = 3 * first + i;
    double current = finite_result(s, w->p[at]),
           previous = finite_result(s, w->previous[at]);
    staged[i] = (float)(current - previous);
    /* Validate the stored float; the public limit is exactly representable. */
    if (!(fabsf(staged[i]) <= (float)H2_LUA_NUMERIC_VALUE_LIMIT))
      luaL_error(s, "prepared numeric result out of bounds");
  }
  memcpy(out->data.f32, staged, 3 * count * sizeof(float));
  return 0;
}
static void validate_coefficient(lua_State *s, const h2_numeric_buffer_t *b,
                                  size_t i) {
  if (b->is_f32) {
    /* The public limit is exactly representable in float. Validate stored
     * coefficients without a software double promotion on single-FPU hosts. */
    float value = b->data.f32[i];
    if (!isfinite(value) || fabsf(value) > (float)H2_LUA_NUMERIC_VALUE_LIMIT)
      luaL_error(s, "prepared numeric result out of bounds");
  } else
    finite_result(s, b->data.f64[i]);
}
static float float_coefficient(h2_numeric_buffer_t *b, size_t i) {
  return b->is_f32 ? b->data.f32[i] : (float)b->data.f64[i];
}
static int workspace_integrate(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  size_t first = node_index(s, h2_numeric_number(s, 2), w->n);
  size_t count = h2_numeric_size(s, 3, w->n - first);
  static const char *const modes[] = {"f64", "displacement-f32", NULL};
  int mode = luaL_checkoption(s, 9, NULL, modes);
  h2_numeric_buffer_t *inputs[6];
  for (int j = 0; j < 5; ++j) {
    size_t extent = j ? 3 * count : count;
    if (mode && j >= 1 && j <= 3) {
      inputs[j] = h2_numeric_check(s, 4 + j);
      h2_numeric_capacity(s, inputs[j], extent);
    } else
      inputs[j] = f64_buffer(s, 4 + j, extent);
  }
  size_t nb = h2_numeric_size(s, 11, 3 * NODE_LIMIT);
  const double *bounds = bounds_read(s, 10, nb, w->n);
  inputs[5] = bounds ? h2_numeric_check(s, 10) : NULL;
  for (int j = 0; j < 6; ++j)
    separate_state(s, w, inputs[j]);
  validate_state(s, w, 1);
  for (int j = 0; j < 6; ++j)
    for (int k = 0; k < j; ++k)
      if (inputs[j] && inputs[j] == inputs[k])
        return luaL_error(s, "aliased integration buffers");
  for (size_t i = 0; i < count; ++i) {
    if (finite_result(s, inputs[0]->data.f64[i]) < 0)
      return luaL_error(s, "negative mobility");
    for (int j = 1; j < 5; ++j)
      for (size_t k = 0; k < 3; ++k)
        validate_coefficient(s, inputs[j], 3 * i + k);
  }
  memcpy(w->staged_p, w->p, 3 * w->n * sizeof(double));
  memcpy(w->staged_previous, w->previous, 3 * w->n * sizeof(double));
  for (size_t i = 0; i < count; ++i)
    if (inputs[0]->data.f64[i] > 0) {
      for (size_t j = 0; j < 3; ++j) {
        size_t k = 3 * i + j, at = 3 * (first + i) + j;
        double old = w->p[at], increment;
        double after = inputs[4]->data.f64[k];
        if (mode) {
          float before = float_coefficient(inputs[1], k),
                g0 = float_coefficient(inputs[2], k),
                g1 = float_coefficient(inputs[3], k);
          float d = (float)(old - w->previous[at]);
          d = (d + before) * g0;
          d = d * g1;
          increment = d;
        } else {
          double before = inputs[1]->data.f64[k], g0 = inputs[2]->data.f64[k],
                 g1 = inputs[3]->data.f64[k];
          increment = ((old - w->previous[at] + before) * g0) * g1;
        }
        w->staged_p[at] = finite_result(s, (old + increment) + after);
        w->staged_previous[at] = old;
      }
    }
  for (size_t k = 0; k < nb; ++k) {
    const double *r = bounds + 5 * k;
    size_t start = (size_t)r[0] - 1, end = start + (size_t)r[1],
           axis = (size_t)r[2] - 1;
    for (size_t i = start; i < end; ++i) {
      if (i < first || i >= first + count ||
          inputs[0]->data.f64[i - first] == 0)
        continue;
      double *v = w->staged_p + 3 * i + axis;
      *v = fmax(r[3], fmin(r[4], *v));
    }
  }
  memcpy(w->p, w->staged_p, 3 * w->n * sizeof(double));
  memcpy(w->previous, w->staged_previous, 3 * w->n * sizeof(double));
  return 0;
}
static void edge_sweep(lua_State *s, constraint_workspace_t *w, size_t i) {
  const prepared_edge_t *e = w->edges + i;
  if (e->predicates & EDGE_FIXED)
    return;
  precise_float *a = w->coordinates + 3 * e->a, *b = w->coordinates + 3 * e->b;
  precise_float x = pf_sub(b[0], a[0]), y = pf_sub(b[1], a[1]),
                z = pf_sub(b[2], a[2]);
  precise_float squared =
      pf_add(pf_add(pf_square(x), pf_square(y)), pf_square(z));
  precise_float difference = pf_sub(squared, e->rest_squared);
  float gap = difference.hi + difference.lo;
  float old = w->staged_lambda[i];
  if (old == 0 && gap < -1e-12f * e->rest_squared.hi)
    return;
  float d = sqrtf(squared.hi);
  if (d < 1e-8f)
    return;
  float strain = h2_f32_div(gap, d + e->rest_f);
  float candidate = old + (-strain - e->alpha * old) * e->inverse;
  if (!isfinite(candidate))
    luaL_error(s, "non-finite constraint intermediate");
  float next = fminf(0, candidate);
  float q = h2_f32_div(next - old, d);
  w->staged_lambda[i] = next;
  float aq = e->weight_a * q, bq = e->weight_b * q;
  precise_float delta[3] = {x, y, z};
  for (size_t j = 0; j < 3; ++j) {
    if (e->predicates & EDGE_MOVE_A)
      a[j] = pf_add(a[j], (precise_float){-aq * delta[j].hi, 0});
    if (e->predicates & EDGE_MOVE_B)
      b[j] = pf_add(b[j], (precise_float){bq * delta[j].hi, 0});
  }
}
static double span_coordinate(constraint_workspace_t *w, size_t at,
                              int pinned, precise_float original) {
  precise_float p = w->coordinates[at];
  /* Preserve the original binary64 pinned endpoint used by the source span. */
  if (pinned && p.hi == original.hi && p.lo == original.lo)
    return w->p[at];
  return (double)p.hi + p.lo;
}
static double span_sweep(constraint_workspace_t *w, double lambda,
                         const precise_float original[6]) {
  const prepared_edge_t *e = &w->span;
  if (!w->has_span || (e->predicates & EDGE_FIXED))
    return lambda;
  double a[3], b[3], delta[3];
  for (size_t j = 0; j < 3; ++j) {
    a[j] = span_coordinate(w, 3 * e->a + j,
                           !(e->predicates & EDGE_MOVE_A), original[j]);
    b[j] = span_coordinate(w, 3 * e->b + j,
                           !(e->predicates & EDGE_MOVE_B), original[3 + j]);
    delta[j] = b[j] - a[j];
  }
  double distance = refined_sqrt(delta[0] * delta[0] + delta[1] * delta[1] +
                                 delta[2] * delta[2]);
  if (distance > e->rest) {
    double alpha = e->alpha64;
    double dl =
        (-(distance - e->rest) - alpha * lambda) / (e->wa + e->wb + alpha);
    lambda += dl;
    double qa = e->wa * dl / distance, qb = e->wb * dl / distance;
    for (size_t j = 0; j < 3; ++j) {
      if (e->predicates & EDGE_MOVE_A)
        w->coordinates[3 * e->a + j] = pf_from(a[j] - qa * delta[j]);
      if (e->predicates & EDGE_MOVE_B)
        w->coordinates[3 * e->b + j] = pf_from(b[j] + qb * delta[j]);
    }
  }
  return lambda;
}
/* Inspect binary32 storage without aliasing or FPU comparisons: some target
 * FPUs may flush subnormals, so those bounds/coordinates keep double compares. */
static int float_subnormal(const float *value) {
  uint32_t bits;
  memcpy(&bits, value, sizeof(bits));
  return (bits & 0x7f800000u) == 0 && (bits & 0x007fffffu) != 0;
}
static int workspace_solve(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  size_t iterations = h2_numeric_size(s, 2, 32);
  if (!iterations)
    return luaL_error(s, "zero constraint iterations");
  size_t nb = h2_numeric_size(s, 4, 3 * NODE_LIMIT);
  const double *bounds = bounds_read(s, 3, nb, w->n);
  if (bounds)
    separate_state(s, w, h2_numeric_check(s, 3));
  validate_state(s, w, 0);
  for (size_t i = 0; i < 3 * w->n; ++i)
    w->coordinates[i] = pf_from(w->p[i]);
  /* These are the already-converted original endpoints. Published p is fixed
   * until commit; refresh this call-local snapshot after every public write. */
  precise_float span_original[6];
  if (w->has_span && !(w->span.predicates & EDGE_FIXED)) {
    memcpy(span_original, w->coordinates + 3 * w->span.a,
           3 * sizeof(precise_float));
    memcpy(span_original + 3, w->coordinates + 3 * w->span.b,
           3 * sizeof(precise_float));
  }
  memcpy(w->staged_lambda, w->lambda, w->m * sizeof(float));
  double span_lambda = w->span_lambda;
  for (size_t pass = 0; pass < iterations; ++pass) {
    for (size_t k = 0; k < w->m; ++k)
      edge_sweep(s, w, (w->sweep + pass) % 2 ? w->m - 1 - k : k);
    span_lambda = span_sweep(w, span_lambda, span_original);
    for (size_t k = 0; k < nb; ++k) {
      const double *r = bounds + 5 * k;
      size_t first = (size_t)r[0] - 1, end = first + (size_t)r[1],
             axis = (size_t)r[2] - 1;
      if (first == end)
        continue;
      float lower = (float)r[3], upper = (float)r[4];
      int exact = !float_subnormal(&lower) && !float_subnormal(&upper) &&
                  (double)lower == r[3] && (double)upper == r[4];
      int fixed = exact ? lower == upper : r[3] == r[4];
      /* Retain the original double endpoint and its signed-zero residual. */
      precise_float low = pf_from(r[3]), high = pf_from(r[4]);
      for (size_t i = first; i < end; ++i) {
        precise_float *v = w->coordinates + 3 * i + axis;
        if (!isfinite(v->hi) || !isfinite(v->lo))
          return luaL_error(s, "non-finite constraint intermediate");
        int use_float = exact && !float_subnormal(&v->hi);
        if (use_float ? v->hi < lower : v->hi < r[3])
          *v = low;
        else if (use_float ? v->hi > upper : v->hi > r[4])
          *v = high;
        else if (fixed)
          *v = low;
      }
    }
  }
  for (size_t i = 0; i < 3 * w->n; ++i)
    w->staged_p[i] =
        finite_result(s, (double)w->coordinates[i].hi + w->coordinates[i].lo);
  for (size_t i = 0; i < w->m; ++i)
    finite_result(s, w->staged_lambda[i]);
  finite_result(s, span_lambda);
  memcpy(w->p, w->staged_p, 3 * w->n * sizeof(double));
  memcpy(w->lambda, w->staged_lambda, w->m * sizeof(float));
  w->span_lambda = span_lambda;
  w->sweep = (w->sweep + iterations) % 2;
  return 0;
}
static int workspace_damp(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  h2_numeric_buffer_t *flags = f64_buffer(s, 2, w->n);
  separate_state(s, w, flags);
  validate_state(s, w, 1);
  const double *mobility = flags->data.f64;
  double blend = h2_numeric_number(s, 3), axial_blend = h2_numeric_number(s, 4);
  double threshold = h2_numeric_number(s, 5), epsilon = h2_numeric_number(s, 6);
  if (blend < 0 || blend > 1 || axial_blend < 0 || axial_blend > 1 ||
      threshold < 0 || epsilon < 0)
    return luaL_error(s, "invalid damping coefficients");
  for (size_t i = 0; i < w->n; ++i)
    if (finite_result(s, mobility[i]) < 0)
      return luaL_error(s, "negative mobility");
  float bf = (float)blend, ab = (float)axial_blend, t = (float)threshold,
        eps = (float)epsilon;
  memcpy(w->staged_previous, w->previous, 3 * w->n * sizeof(double));
  for (size_t i = 0; i < 3 * w->n; ++i)
    w->displacement[i] = (float)(w->p[i] - w->previous[i]);
  for (size_t i = 1; i + 1 < w->n; ++i)
    if (mobility[i] > 0)
      for (size_t j = 0; j < 3; ++j) {
        size_t k = 3 * i + j;
        float mean = (w->displacement[k - 3] + w->displacement[k + 3]) * .5f;
        w->staged_previous[k] -= (double)((mean - w->displacement[k]) * bf);
      }
  for (size_t i = 0; i < w->m; ++i) {
    const prepared_edge_t *e = w->edges + i;
    if (e->predicates & EDGE_FIXED)
      continue;
    const double *a = w->p + 3 * e->a, *b = w->p + 3 * e->b;
    double *ap = w->staged_previous + 3 * e->a,
           *bp = w->staged_previous + 3 * e->b;
    float x = (float)(b[0] - a[0]), y = (float)(b[1] - a[1]),
          z = (float)(b[2] - a[2]);
    float squared = x * x + y * y + z * z;
    if (squared >= e->rest_f * e->rest_f * (t * t) && squared > eps * eps) {
      float axial = (float)(b[0] - bp[0] - a[0] + ap[0]) * x +
                    (float)(b[1] - bp[1] - a[1] + ap[1]) * y +
                    (float)(b[2] - bp[2] - a[2] + ap[2]) * z;
      if (axial > 0) {
        float impulse = axial * ab / (e->weight_a + e->weight_b) / squared;
        float ia = e->weight_a * impulse, ib = e->weight_b * impulse;
        float delta[3] = {x, y, z};
        for (size_t j = 0; j < 3; ++j) {
          ap[j] -= (double)(ia * delta[j]);
          bp[j] += (double)(ib * delta[j]);
        }
      }
    }
  }
  for (size_t i = 0; i < 3 * w->n; ++i)
    finite_result(s, w->staged_previous[i]);
  memcpy(w->previous, w->staged_previous, 3 * w->n * sizeof(double));
  return 0;
}
static int workspace_copy(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  h2_numeric_buffer_t *p = f64_buffer(s, 2, 3 * w->n),
                      *prev = f64_buffer(s, 3, 3 * w->n),
                      *lambda = f64_buffer(s, 4, w->m);
  if (p == prev || p == lambda || prev == lambda)
    return luaL_error(s, "aliased output buffers");
  separate_state(s, w, p);
  separate_state(s, w, prev);
  separate_state(s, w, lambda);
  memcpy(p->data.f64, w->p, 3 * w->n * sizeof(double));
  memcpy(prev->data.f64, w->previous, 3 * w->n * sizeof(double));
  for (size_t i = 0; i < w->m; ++i)
    lambda->data.f64[i] = w->lambda[i];
  lua_pushnumber(s, w->span_lambda);
  return 1;
}
static int workspace_multipliers(lua_State *s) {
  constraint_workspace_t *w = workspace(s);
  loaded(s, w);
  if (lua_isnoneornil(s, 2))
    lua_pushnil(s);
  else {
    size_t i = node_index(s, h2_numeric_number(s, 2), w->m);
    lua_pushnumber(s, w->lambda[i]);
  }
  lua_pushnumber(s, w->span_lambda);
  return 2;
}
void h2_numeric_prepared_register(lua_State *s) {
  if (luaL_newmetatable(s, WORK_META)) {
    static const luaL_Reg methods[] = {
        {"begin", workspace_begin},
        {"node", workspace_node},   {"edge", workspace_edge},
        {"span", workspace_span},   {"integrate", workspace_integrate},
        {"solve", workspace_solve}, {"damp", workspace_damp},
        {"copy", workspace_copy}, {"multipliers", workspace_multipliers},
        {"displacements", workspace_displacements},
        {NULL, NULL}};
    luaL_setfuncs(s, methods, 0);
    lua_pushboolean(s, 0);
    lua_pushcclosure(s, workspace_load, 1);
    lua_setfield(s, -2, "load");
    lua_pushboolean(s, 1);
    lua_pushcclosure(s, workspace_load, 1);
    lua_setfield(s, -2, "bind");
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "constraint workspace");
    lua_setfield(s, -2, "__metatable");
  }
  lua_pop(s, 1);
  lua_pushcfunction(s, workspace_new);
  lua_setfield(s, -2, "constraints");
  lua_pushcfunction(s, length3_refined);
  lua_setfield(s, -2, "length3_refined");
}
