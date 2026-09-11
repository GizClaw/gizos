#include "h2_lua_vector_sw.h"
#include "h2_lua_vector_delta.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Four vertical samples and analytic horizontal coverage. The path is kept in
 * object coordinates so transforms also apply to stroke widths and gradients.
 */
#define MAX_POINTS 65536u
#define MAX_EDGES 131072u
#define EDGE_BLOCK 2048u
#define EDGE_BLOCKS (MAX_EDGES / EDGE_BLOCK)
#define MAX_WORKSPACE (4u * 1024u * 1024u)
#define PI 3.14159265358979323846

typedef struct {
  const uint8_t *p, *end;
  int valid;
} reader_t;
typedef struct {
  float x, y;
  unsigned char move, close;
} point_t;
typedef struct {
  float x0, y0, x1, y1;
} edge_t;
typedef struct {
  float x;
  int winding;
} crossing_t;
typedef struct {
  float offset, color[4];
} stop_t;
typedef struct {
  unsigned radial, count;
  float xy[4];
  stop_t stops[16];
} gradient_t;
typedef struct {
  double matrix[6];
  uint8_t *clip;
} state_t;
#define COVERAGE_CACHE_SLOTS 1024u
#define COVERAGE_CACHE_PROBES 16u
typedef struct {
  double matrix[6], line_width;
  size_t offset, bytes;
  uint32_t hash;
  unsigned points, stroke, width, height, x, y, w, h, valid;
} coverage_entry_t;
struct h2_lua_vector_sw_cache {
  size_t capacity, cursor, scratch_bytes;
  unsigned next;
  coverage_entry_t entries[COVERAGE_CACHE_SLOTS];
  /* Point keys precede rows of (first, count) and exact float coverage.
   * The tail of the same allocation stages one contour before publication. */
  uint8_t payload[];
};
size_t h2_lua_vector_sw_cache_bytes(size_t payload_bytes) {
  if (payload_bytes < 4096 || payload_bytes > 16u * 1024u * 1024u)
    return 0;
  return sizeof(h2_lua_vector_sw_cache_t) + (payload_bytes & ~(size_t)7);
}
h2_lua_vector_sw_cache_t *h2_lua_vector_sw_cache_init(void *storage, size_t bytes) {
  if (!storage || bytes < sizeof(h2_lua_vector_sw_cache_t) + 4096 ||
      bytes - sizeof(h2_lua_vector_sw_cache_t) > 16u * 1024u * 1024u)
    return NULL;
  h2_lua_vector_sw_cache_t *cache = storage;
  memset(cache, 0, sizeof(*cache));
  size_t payload = (bytes - sizeof(*cache)) & ~(size_t)7;
  cache->scratch_bytes = (payload / 8u) & ~(size_t)7;
  if(cache->scratch_bytes > 32768u)cache->scratch_bytes = 32768u;
  cache->capacity = payload - cache->scratch_bytes;
  return cache;
}
typedef struct {
  unsigned width, height, points, point_capacity, edges, edge_capacity;
  size_t allocated;
  int valid, has_path;
  h2_lua_vector_sw_result_t failure;
  h2_lua_vector_sw_poll_fn poll;
  void *poll_user;
  point_t *path;
  edge_t *edge_blocks[EDGE_BLOCKS];
  crossing_t *crossings;
  unsigned crossing_capacity;
  float *coverage;
  unsigned *active, active_capacity;
  double *unit_circle;
  unsigned circle_count;
  uint8_t *rgba, *clips[32];
  uint8_t *const *rows;
  unsigned clip_count;
  double bounds[4];
  h2_lua_vector_sw_cache_t *cache;
  uint32_t cache_hash;
  unsigned cache_stroke;
  double cache_line_width;
  int cache_record;
} render_t;

/* Inputs are validated finite; coverage and colors stay bounded. Avoid
 * out-of-line NaN-aware libm min/max in the per-sample and per-pixel loops. */
static inline float min_float(float a, float b) { return a < b ? a : b; }
static inline float max_float(float a, float b) { return a > b ? a : b; }

static int poll_render(render_t *r) {
  if (r->valid && r->poll && !r->poll(r->poll_user)) {
    r->valid = 0;r->failure = H2_LUA_VECTOR_SW_INTERRUPTED;
  }
  return r->valid;
}

static unsigned byte(reader_t *r) {
  if (r->p == r->end) {
    r->valid = 0;
    return 0;
  }
  return *r->p++;
}
static unsigned word(reader_t *r) {
  unsigned a = byte(r);
  return a | (byte(r) << 8);
}
static double number(reader_t *r) {
  uint32_t bits = byte(r);
  bits |= byte(r) << 8;
  bits |= byte(r) << 16;
  bits |= byte(r) << 24;
  float value;
  memcpy(&value, &bits, sizeof(value));
  if (!isfinite(value) || fabs(value) > 1000000) {
    r->valid = 0;
    return 0;
  }
  return value;
}
static void *grow(render_t *r, void *p, size_t old_size, size_t new_size) {
  if (new_size > MAX_WORKSPACE - (r->allocated - old_size)) {
    r->valid = 0;r->failure = H2_LUA_VECTOR_SW_WORKSPACE_LIMIT;
    return NULL;
  }
  void *q = realloc(p, new_size);
  if (!q) {
    r->valid = 0;r->failure = H2_LUA_VECTOR_SW_NO_MEMORY;
    return NULL;
  }
  r->allocated += new_size - old_size;
  return q;
}
static void point(render_t *r, double x, double y, int move) {
  if (!r->valid || (!(r->points % 1024) && !poll_render(r)))
    return;
  if (r->points == r->point_capacity) {
    unsigned n = r->point_capacity ? r->point_capacity * 2 : 128;
    if (n > MAX_POINTS) {
      r->valid = 0;
      return;
    }
    void *p = grow(r, r->path, r->point_capacity * sizeof(point_t),
                   n * sizeof(point_t));
    if (!p)
      return;
    r->path = p;
    r->point_capacity = n;
  }
  r->path[r->points++] = (point_t){(float)x, (float)y, (unsigned char)move, 0};
}
static void curve(render_t *r, double x0, double y0, const double v[6],
                  double tolerance, unsigned depth) {
  double dx = v[4] - x0, dy = v[5] - y0;
  double d1 = fabs((v[0] - x0) * dy - (v[1] - y0) * dx);
  double d2 = fabs((v[2] - x0) * dy - (v[3] - y0) * dx);
  double span = hypot(dx, dy);
  if (depth == 12 ||
      (span > 1e-8 && d1 + d2 <= tolerance * span &&
       hypot(v[0] - x0, v[1] - y0) + hypot(v[2] - v[0], v[3] - v[1]) +
               hypot(v[4] - v[2], v[5] - v[3]) <=
           span + tolerance)) {
    point(r, v[4], v[5], 0);
    return;
  }
  double ax = (x0 + v[0]) * .5, ay = (y0 + v[1]) * .5, bx = (v[0] + v[2]) * .5,
         by = (v[1] + v[3]) * .5;
  double cx = (v[2] + v[4]) * .5, cy = (v[3] + v[5]) * .5;
  double ux = (ax + bx) * .5, uy = (ay + by) * .5, vx = (bx + cx) * .5,
         vy = (by + cy) * .5;
  double mx = (ux + vx) * .5, my = (uy + vy) * .5;
  double left[] = {ax, ay, ux, uy, mx, my},
         right[] = {vx, vy, cx, cy, v[4], v[5]};
  curve(r, x0, y0, left, tolerance, depth + 1);
  if (r->valid)
    curve(r, mx, my, right, tolerance, depth + 1);
}
static inline edge_t *edge_at(const render_t *r, unsigned index) {
  return &r->edge_blocks[index / EDGE_BLOCK][index % EDGE_BLOCK];
}
static void edge(render_t *r, const double m[6], double x0, double y0,
                 double x1, double y1) {
  if (!r->valid || (!(r->edges % 1024) && !poll_render(r)))
    return;
  double a = m[0] * x0 + m[2] * y0 + m[4], b = m[1] * x0 + m[3] * y0 + m[5];
  double c = m[0] * x1 + m[2] * y1 + m[4], d = m[1] * x1 + m[3] * y1 + m[5];
  if (fabs(d - b) < 1e-10)
    return;
  /* The raster samples only y = (sample + .5)/4. Tiny stroke-join edges
   * often intersect none of those rows. Such edges cannot affect winding,
   * coverage or clipping; discard them before allocating/sorting workspace.
   * Test the stored float endpoints, exactly as the scan converter does. */
  float stored_y0 = (float)b, stored_y1 = (float)d;
  float lower = min_float(stored_y0, stored_y1), upper = max_float(stored_y0, stored_y1);
  if (upper <= .125f || lower > r->height - .125f) return;
  float first_sample = max_float(0, ceilf((lower - .125f) * 4));
  if (first_sample * .25f + .125f >= upper) return;
  if (r->edges == r->edge_capacity) {
    if (r->edge_capacity == MAX_EDGES) {
      r->valid = 0;
      return;
    }
    /* Fixed blocks avoid both a large contiguous allocation and realloc's
     * temporary old/new copies on the device's fragmented PSRAM heap. */
    void *p = grow(r, NULL, 0, EDGE_BLOCK * sizeof(edge_t));
    if (!p) return;
    r->edge_blocks[r->edge_capacity / EDGE_BLOCK] = p;
    r->edge_capacity += EDGE_BLOCK;
  }
  *edge_at(r, r->edges++) = (edge_t){(float)a, (float)b, (float)c, (float)d};
}
static void stroke_line(render_t *r, const double m[6], point_t a, point_t b,
                        double radius) {
  double dx = b.x - a.x, dy = b.y - a.y, len = hypot(dx, dy);
  if (len < 1e-10)
    return;
  double x = dy / len * radius, y = -dx / len * radius;
  edge(r, m, a.x + x, a.y + y, b.x + x, b.y + y);
  edge(r, m, b.x + x, b.y + y, b.x - x, b.y - y);
  edge(r, m, b.x - x, b.y - y, a.x - x, a.y - y);
  edge(r, m, a.x - x, a.y - y, a.x + x, a.y + y);
}
static void circle(render_t *r, const double m[6], point_t p, double radius) {
  double scale = fmax(hypot(m[0], m[1]), hypot(m[2], m[3]));
  unsigned n = (unsigned)fmin(
      128, fmax(12, ceil(PI * sqrt(fmax(1, radius * scale) * 2))));
  /* A stroke uses the same unit polygon at every vertex. Reuse its exact
   * double sin/cos values instead of repeating transcendental work per join. */
  if (!r->unit_circle) {
    r->unit_circle = grow(r, NULL, 0, 128 * 2 * sizeof(double));
    if (!r->unit_circle) return;
  }
  if (r->circle_count != n) {
    for (unsigned i = 1; i <= n; i++) {
      double t = 2 * PI * i / n;
      r->unit_circle[(i - 1) * 2] = cos(t);
      r->unit_circle[(i - 1) * 2 + 1] = sin(t);
    }
    r->circle_count = n;
  }
  double x = p.x + radius, y = p.y;
  for (unsigned i = 0; i < n; i++) {
    double nx = p.x + radius * r->unit_circle[i * 2];
    double ny = p.y + radius * r->unit_circle[i * 2 + 1];
    edge(r, m, x, y, nx, ny);
    x = nx;
    y = ny;
  }
}
static void make_edges(render_t *r, const double m[6], int stroke,
                       double width) {
  r->edges = 0;
  r->bounds[0] = r->bounds[1] = 1000000;
  r->bounds[2] = r->bounds[3] = -1000000;
  unsigned first = 0;
  for (unsigned i = 0; i < r->points && r->valid; i++) {
    point_t p = r->path[i];
    r->bounds[0] = fmin(r->bounds[0], p.x);
    r->bounds[1] = fmin(r->bounds[1], p.y);
    r->bounds[2] = fmax(r->bounds[2], p.x);
    r->bounds[3] = fmax(r->bounds[3], p.y);
    if (p.move)
      first = i;
    if (stroke && width > 0)
      circle(r, m, p, width * .5);
    int last = (i + 1 == r->points || r->path[i + 1].move);
    if (!last || p.close || !stroke) {
      point_t q = r->path[last ? first : i + 1];
      if (stroke) {
        if (width > 0)
          stroke_line(r, m, p, q, width * .5);
      } else
        edge(r, m, p.x, p.y, q.x, q.y);
    }
  }
}
/* Sort once per path. Only edges intersecting the current subpixel row enter
 * the crossing sort, rather than revisiting the entire path four times/row. */
static int compare_edge_start(const void *a, const void *b) {
  const edge_t *x = a, *y = b;
  float sx = min_float(x->y0, x->y1), sy = min_float(y->y0, y->y1);
  return (sx > sy) - (sx < sy);
}
static void edge_heap_down(const render_t *r, unsigned *heap, unsigned count,
                           unsigned root) {
  for (;;) {
    unsigned child = root * 2 + 1;
    if (child >= count) return;
    if (child + 1 < count &&
        compare_edge_start(edge_at(r, heap[child + 1]), edge_at(r, heap[child])) < 0)
      child++;
    if (compare_edge_start(edge_at(r, heap[root]), edge_at(r, heap[child])) <= 0)
      return;
    unsigned value = heap[root];
    heap[root] = heap[child];
    heap[child] = value;
    root = child;
  }
}
static int compare_crossing(const void *a, const void *b) {
  float x = ((const crossing_t *)a)->x, y = ((const crossing_t *)b)->x;
  return (x > y) - (x < y);
}
static void cover(render_t *r, float left, float right) {
  left = max_float(0, left);
  right = min_float(r->width, right);
  if (right <= left)
    return;
  unsigned a = (unsigned)floorf(left), b = (unsigned)ceilf(right);
  for (unsigned x = a; x < b; x++)
    r->coverage[x] += (min_float(right, x + 1.f) - max_float(left, x)) * .25f;
}
static void gradient_color(const gradient_t *g, double px, double py,
                           float out[4]) {
  float x = (float)px, y = (float)py, t;
  if (fabs(px) > 1e12 || fabs(py) > 1e12) {
    double value;
    if (g->radial) value = hypot(px - g->xy[0], py - g->xy[1]) / g->xy[2];
    else {
      double dx = (double)g->xy[2] - g->xy[0], dy = (double)g->xy[3] - g->xy[1];
      double d = dx * dx + dy * dy;
      value = d > 1e-20 ? ((px - g->xy[0]) * dx + (py - g->xy[1]) * dy) / d : 0;
    }
    t = (float)fmax(0, fmin(1, value));
  } else if (g->radial)
    t = hypotf(x - g->xy[0], y - g->xy[1]) / g->xy[2];
  else {
    float dx = g->xy[2] - g->xy[0], dy = g->xy[3] - g->xy[1],
          d = dx * dx + dy * dy;
    t = d > 1e-20f ? ((x - g->xy[0]) * dx + (y - g->xy[1]) * dy) / d : 0;
  }
  unsigned j = 0;
  while (j + 1 < g->count && t > g->stops[j + 1].offset)
    j++;
  const stop_t *a = &g->stops[j], *b = &g->stops[j + 1 < g->count ? j + 1 : j];
  float d = b->offset - a->offset,
        u = d > 0 ? min_float(1, max_float(0, (t - a->offset) / d)) : 0;
  for (unsigned k = 0; k < 4; k++)
    out[k] = a->color[k] * (1 - u) + b->color[k] * u;
}
static uint32_t coverage_hash_bytes(uint32_t hash, const void *data, size_t bytes) {
  const uint8_t *p = data;
  for (size_t i = 0; i < bytes; i++) hash = (hash ^ p[i]) * 16777619u;
  return hash;
}
static coverage_entry_t *coverage_find(render_t *r, const state_t *state,
                                       unsigned stroke, double line_width) {
  if (!r->cache || !r->points || r->points > 256) return NULL;
  uint32_t hash = coverage_hash_bytes(2166136261u, state->matrix, sizeof(state->matrix));
  hash = coverage_hash_bytes(hash, &line_width, sizeof(line_width));
  hash = (hash ^ stroke) * 16777619u;
  hash = (hash ^ r->width) * 16777619u;
  hash = (hash ^ r->height) * 16777619u;
  for (unsigned i = 0; i < r->points; i++) {
    hash = coverage_hash_bytes(hash, &r->path[i].x, sizeof(float));
    hash = coverage_hash_bytes(hash, &r->path[i].y, sizeof(float));
    hash = (hash ^ r->path[i].move) * 16777619u;
    hash = (hash ^ r->path[i].close) * 16777619u;
  }
  r->cache_hash = hash;
  r->cache_stroke = stroke;
  r->cache_line_width = line_width;
  r->cache_record = 1;
  for (unsigned probe = 0; probe < COVERAGE_CACHE_PROBES; probe++) {
    coverage_entry_t *e = &r->cache->entries[(hash + probe) % COVERAGE_CACHE_SLOTS];
    if (!e->valid || e->hash != hash || e->points != r->points ||
        e->stroke != stroke || e->line_width != line_width ||
        e->width != r->width || e->height != r->height ||
        memcmp(e->matrix, state->matrix, sizeof(e->matrix))) continue;
    const point_t *points = (const point_t *)(r->cache->payload + e->offset);
    unsigned i = 0;
    for (; i < r->points; i++) {
      /* Ignore struct padding. Hash collisions never stand in for equality. */
      if (memcmp(&points[i].x, &r->path[i].x, sizeof(float)) ||
          memcmp(&points[i].y, &r->path[i].y, sizeof(float)) ||
          points[i].move != r->path[i].move || points[i].close != r->path[i].close)
        break;
    }
    if (i == r->points) return e;
  }
  return NULL;
}
static uint8_t *coverage_pixels(h2_lua_vector_sw_cache_t *cache, coverage_entry_t *e) {
  return cache->payload + e->offset + e->points * sizeof(point_t);
}
static coverage_entry_t *coverage_reserve(render_t *r, const state_t *state,
                                         unsigned x, unsigned y, unsigned w, unsigned h, size_t coverage_bytes) {
  if (!r->cache_record) return NULL;
  h2_lua_vector_sw_cache_t *cache = r->cache;
  size_t bytes = (r->points * sizeof(point_t) + coverage_bytes + 7) & ~(size_t)7;
  /* Large backdrops should not evict hundreds of small light contours. */
  if (bytes > 32768 || bytes > cache->capacity) return NULL;
  if (bytes > cache->capacity - cache->cursor) cache->cursor = 0;
  size_t start = cache->cursor, end = start + bytes;
  for (unsigned i = 0; i < COVERAGE_CACHE_SLOTS; i++) {
    coverage_entry_t *e = &cache->entries[i];
    if (e->valid && e->offset < end && e->offset + e->bytes > start) e->valid = 0;
  }
  coverage_entry_t *entry = NULL;
  for (unsigned probe = 0; probe < COVERAGE_CACHE_PROBES; probe++) {
    coverage_entry_t *e = &cache->entries[(r->cache_hash + probe) % COVERAGE_CACHE_SLOTS];
    if (!e->valid) { entry = e; break; }
  }
  if (!entry)
    entry = &cache->entries[(r->cache_hash + cache->next++ % COVERAGE_CACHE_PROBES) % COVERAGE_CACHE_SLOTS];
  *entry = (coverage_entry_t){.line_width = r->cache_line_width,
      .offset = start, .bytes = bytes, .hash = r->cache_hash, .points = r->points,
      .stroke = r->cache_stroke, .width = r->width, .height = r->height,
      .x = x, .y = y, .w = w, .h = h};
  memcpy(entry->matrix, state->matrix, sizeof(entry->matrix));
  memcpy(cache->payload + start, r->path, r->points * sizeof(point_t));
  cache->cursor = end;
  return entry; /* Published only after every coverage row is complete. */
}
static void raster_cached(render_t *r, const state_t *state, const double color[4],
                           double opacity, coverage_entry_t *entry) {
  const uint8_t *pixels = coverage_pixels(r->cache, entry);
  float c[4];
  for (unsigned k = 0; k < 4; k++) c[k] = (float)color[k];
  float alpha_scale = (float)opacity;
  for (unsigned row = 0; row < entry->h; row++) {
    unsigned y = entry->y + row;
    if (!(y % 16) && !poll_render(r)) return;
    uint32_t span[2];memcpy(span,pixels,sizeof(span));pixels+=sizeof(span);
    const float *values=(const float *)pixels;pixels+=(size_t)span[1]*sizeof(float);
    for (unsigned column = 0; column < span[1]; column++) {
      unsigned x = entry->x + span[0] + column;
      size_t p = (size_t)y * r->width + x;
      float coverage = values[column];
      if (state->clip) coverage *= state->clip[p] * (1.f / 255.f);
      if (coverage <= 0) continue;
      float a = c[3] * alpha_scale * coverage;
      uint8_t *pixel = r->rows ? r->rows[y] + x * 4 : r->rgba + p * 4;
      for (unsigned k = 0; k < 3; k++)
        pixel[k] = (uint8_t)min_float(255, floorf(c[k] * 255 * a + pixel[k] * (1 - a) + .5f));
      pixel[3] = (uint8_t)min_float(255, floorf(255 * a + pixel[3] * (1 - a) + .5f));
    }
  }
}
static void raster(render_t *r, const state_t *state, const gradient_t *g,
                   const double color[4], double opacity, uint8_t *mask) {
  if (!r->valid || !r->edges)
    return;
  const double *m = state->matrix;
  double determinant = m[0] * m[3] - m[1] * m[2];
  double bw = r->bounds[2] - r->bounds[0], bh = r->bounds[3] - r->bounds[1];
  /* Invert once, not twice per covered pixel. Keep affine setup in double;
   * bounded pixel coverage and color arithmetic use single precision. */
  double inv[6] = {0};
  if (g && bw > 0 && bh > 0) {
    inv[0] = m[3] / determinant / bw;
    inv[1] = -m[2] / determinant / bw;
    inv[2] = (-m[3] * m[4] + m[2] * m[5]) / determinant / bw - r->bounds[0] / bw;
    inv[3] = -m[1] / determinant / bh;
    inv[4] = m[0] / determinant / bh;
    inv[5] = (m[1] * m[4] - m[0] * m[5]) / determinant / bh - r->bounds[1] / bh;
  }
  float solid[4];
  for (unsigned k = 0; k < 4; k++) solid[k] = (float)color[k];
  float alpha_scale = (float)opacity;
  float xmin = r->width, xmax = 0, ymin = r->height, ymax = 0;
  for (unsigned i = 0; i < r->edges; i++) {
    const edge_t *e = edge_at(r, i);
    xmin = min_float(xmin, min_float(e->x0, e->x1));
    xmax = max_float(xmax, max_float(e->x0, e->x1));
    ymin = min_float(ymin, min_float(e->y0, e->y1));
    ymax = max_float(ymax, max_float(e->y0, e->y1));
  }
  unsigned left_x = (unsigned)max_float(0, min_float(r->width, floorf(xmin)));
  unsigned right_x = (unsigned)max_float(0, min_float(r->width, ceilf(xmax)));
  unsigned start = (unsigned)max_float(0, min_float(r->height, floorf(ymin)));
  unsigned end = (unsigned)max_float(0, min_float(r->height, ceilf(ymax)));
  if (left_x >= right_x || start >= end) return;
  uint8_t *record_pixels = r->cache_record ? r->cache->payload+r->cache->capacity : NULL;
  size_t record_bytes=0;
  /* Merge sorted blocks with a tiny heap instead of flattening the edges. */
  unsigned heap[EDGE_BLOCKS], heap_count = (r->edges + EDGE_BLOCK - 1) / EDGE_BLOCK;
  for (unsigned i = 0; i < heap_count; i++) {
    unsigned base = i * EDGE_BLOCK, count = r->edges - base;
    if (count > EDGE_BLOCK) count = EDGE_BLOCK;
    qsort(r->edge_blocks[i], count, sizeof(edge_t), compare_edge_start);
    heap[i] = base;
  }
  for (unsigned i = heap_count / 2; i > 0; i--)
    edge_heap_down(r, heap, heap_count, i - 1);
  unsigned active = 0;
  for (unsigned y = start; y < end; y++) {
    if (!(y % 16) && !poll_render(r)) return;
    memset(r->coverage + left_x, 0, (right_x - left_x) * sizeof(float));
    for (unsigned sample = 0; sample < 4; sample++) {
      float sy = y + (sample + .5f) * .25f;
      while (heap_count &&
             min_float(edge_at(r, heap[0])->y0, edge_at(r, heap[0])->y1) <= sy) {
        unsigned index = heap[0], next = index + 1;
        if (next < r->edges && next % EDGE_BLOCK)
          heap[0] = next;
        else
          heap[0] = heap[--heap_count];
        if (heap_count) edge_heap_down(r, heap, heap_count, 0);
        /* Only edges crossing this scanline need active/crossing storage.
         * Complex contour banks have tens of thousands of total edges but
         * far fewer simultaneous intersections. Keep every sample unchanged. */
        if (active == r->active_capacity) {
          unsigned capacity = r->active_capacity ? r->active_capacity * 2 : 128;
          void *p = grow(r, r->active, r->active_capacity * sizeof(unsigned),
                         capacity * sizeof(unsigned));
          if (!p) return;
          r->active = p;
          r->active_capacity = capacity;
        }
        r->active[active++] = index;
      }
      if (r->crossing_capacity < active) {
        unsigned capacity = r->active_capacity;
        void *p = grow(r, r->crossings, r->crossing_capacity * sizeof(crossing_t),
                       capacity * sizeof(crossing_t));
        if (!p) return;
        r->crossings = p;
        r->crossing_capacity = capacity;
      }
      unsigned n = 0, kept = 0;
      for (unsigned i = 0; i < active; i++) {
        unsigned index = r->active[i];
        edge_t e = *edge_at(r, index);
        if (sy >= max_float(e.y0, e.y1)) continue;
        r->active[kept++] = index;
        /* Preserve the original intersection rounding at steep/tiny edges. */
        r->crossings[n++] = (crossing_t){
            (float)(e.x0 + ((double)sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0)),
            e.y1 > e.y0 ? 1 : -1};
      }
      active = kept;
      if (n > 1) qsort(r->crossings, n, sizeof(crossing_t), compare_crossing);
      int winding = 0;
      float left = 0;
      for (unsigned i = 0; i < n; i++) {
        if (winding)
          cover(r, left, r->crossings[i].x);
        winding += r->crossings[i].winding;
        left = r->crossings[i].x;
      }
    }
    if(record_pixels) {
      unsigned first=left_x,last=right_x;
      while(first<last && r->coverage[first]<=0)first++;
      while(last>first && r->coverage[last-1]<=0)last--;
      size_t bytes=sizeof(uint32_t)*2+(size_t)(last-first)*sizeof(float);
      if(bytes>r->cache->scratch_bytes-record_bytes)record_pixels=NULL;
      else {
        uint32_t span[2]={first-left_x,last-first};
        memcpy(record_pixels+record_bytes,span,sizeof(span));record_bytes+=sizeof(span);
        float *values=(float *)(record_pixels+record_bytes);
        for(unsigned x=first;x<last;x++)values[x-first]=min_float(1,r->coverage[x]);
        record_bytes+=(size_t)(last-first)*sizeof(float);
      }
    }
    for (unsigned x = left_x; x < right_x; x++) {
      size_t p = (size_t)y * r->width + x;
      float coverage = min_float(1, r->coverage[x]);
      if (state->clip)
        coverage *= state->clip[p] * (1.f / 255.f);
      if (mask) {
        mask[p] = (uint8_t)floorf(coverage * 255 + .5f);
        continue;
      }
      if (coverage <= 0)
        continue;
      float c[4];
      memcpy(c, solid, sizeof(c));
      if (g) {
        if (bw <= 0 || bh <= 0)
          continue;
        gradient_color(g, inv[0] * (x + .5) + inv[1] * (y + .5) + inv[2],
                       inv[3] * (x + .5) + inv[4] * (y + .5) + inv[5], c);
      }
      float a = c[3] * alpha_scale * coverage;
      uint8_t *pixel = r->rows ? r->rows[y] + x * 4 : r->rgba + p * 4;
      for (unsigned k = 0; k < 3; k++)
        pixel[k] = (uint8_t)min_float(
            255, floorf(c[k] * 255 * a + pixel[k] * (1 - a) + .5f));
      pixel[3] = (uint8_t)min_float(
          255, floorf(255 * a + pixel[3] * (1 - a) + .5f));
    }
  }
  if(record_pixels && r->valid) {
    coverage_entry_t *record=coverage_reserve(r,state,left_x,start,right_x-left_x,end-start,record_bytes);
    if(record) {
      memcpy(coverage_pixels(r->cache,record),record_pixels,record_bytes);
      record->valid=1;
    }
  }
}

h2_lua_vector_sw_result_t h2_lua_vector_sw_render_cached_result(const uint8_t *data, size_t length,
    uint8_t *rgba, uint8_t *const *rows, unsigned width, unsigned height,
    const double matrix[6], h2_lua_vector_sw_poll_fn poll, void *user,
    h2_lua_vector_sw_cache_t *cache) {
  if (!data || (!rgba && !rows) || (rgba && rows) || !matrix || length < 12 || length > 2000000 ||
      memcmp(data, "H2VG", 4) || !width || !height || width > 4096 ||
      height > 4096)
    return 0;
  if (rows) for (unsigned y = 0; y < height; y++) if (!rows[y]) return 0;
  for (unsigned i = 0; i < 6; i++)
    if (!isfinite(matrix[i]) || fabs(matrix[i]) > 1000000)
      return 0;
  if (fabs(matrix[0] * matrix[3] - matrix[1] * matrix[2]) < 1e-12)
    return 0;
  reader_t reader = {data + 4, data + length, 1}, *s = &reader;
  unsigned vw = word(s), vh = word(s), count = word(s), version = word(s);
  if (!vw || !vh || vw > 4096 || vh > 4096 || count > 16 ||
      (version != 1 && version != 2))
    return 0;
  /* Gradient and saved-state tables live in bounded heap, not the task stack.
   */
  gradient_t *gradients = calloc(16, sizeof(gradient_t));
  state_t *states = calloc(33, sizeof(state_t));
  render_t r = {.width = width, .height = height, .rgba = rgba, .rows = rows, .valid = 1,
                .poll = poll, .poll_user = user, .cache = cache};
  r.coverage = calloc(width, sizeof(float));
  int success = 0;
  unsigned depth = 0, commands = 0;
  if (!gradients || !states || !r.coverage) {
    r.failure = H2_LUA_VECTOR_SW_NO_MEMORY;goto done;
  }
  memcpy(states[0].matrix, matrix, 6 * sizeof(double));
  for (unsigned i = 0; i < count; i++) {
    gradient_t *g = &gradients[i];
    g->radial = byte(s);
    g->count = byte(s);
    for (unsigned j = 0; j < 4; j++)
      g->xy[j] = number(s);
    if (g->radial > 1 || !g->count || g->count > 16 ||
        (g->radial && g->xy[2] <= 0))
      goto done;
    double previous = -1;
    for (unsigned j = 0; j < g->count; j++) {
      stop_t *stop = &g->stops[j];
      stop->offset = number(s);
      if (stop->offset < 0 || stop->offset > 1 || stop->offset < previous)
        goto done;
      previous = stop->offset;
      for (unsigned k = 0; k < 4; k++)
        stop->color[k] = byte(s) / 255.;
    }
  }
  if (rows) for (unsigned y = 0; y < height; y++) memset(rows[y], 0, (size_t)width * 4);
  else memset(rgba, 0, (size_t)width * height * 4);
  while (s->valid && r.valid && s->p < s->end && ++commands < 65536) {
    if (!(commands % 64) && !poll_render(&r)) goto done;
    unsigned opcode = byte(s);
    double v[6];
    state_t *state = &states[depth];
    switch (opcode) {
    case 0:
      success = s->valid && depth == 0 && s->p == s->end;
      goto done;
    case 1:
      if (depth >= 32)
        goto done;
      states[depth + 1] = *state;
      depth++;
      break;
    case 2:
      if (!depth)
        goto done;
      depth--;
      break;
    case 3: {
      for (unsigned i = 0; i < 6; i++)
        v[i] = number(s);
      double *m = state->matrix, out[] = {m[0] * v[0] + m[2] * v[1],
                                          m[1] * v[0] + m[3] * v[1],
                                          m[0] * v[2] + m[2] * v[3],
                                          m[1] * v[2] + m[3] * v[3],
                                          m[0] * v[4] + m[2] * v[5] + m[4],
                                          m[1] * v[4] + m[3] * v[5] + m[5]};
      if (!s->valid || fabs(out[0] * out[3] - out[1] * out[2]) < 1e-12)
        goto done;
      for (unsigned i = 0; i < 6; i++)
        if (!isfinite(out[i]) || fabs(out[i]) > 100000000)
          goto done;
      memcpy(m, out, sizeof(out));
      break;
    }
    case 4:
      r.points = 0;
      r.has_path = 1;
      break;
    case 5:
    case 6:
      v[0] = number(s);
      v[1] = number(s);
      if (!r.has_path || !s->valid)
        goto done;
      point(&r, v[0], v[1], opcode == 5 || !r.points);
      break;
    case 7: {
      for (unsigned i = 0; i < 6; i++)
        v[i] = number(s);
      if (!r.has_path || !r.points || !s->valid)
        goto done;
      double scale = fmax(hypot(state->matrix[0], state->matrix[1]),
                          hypot(state->matrix[2], state->matrix[3]));
      point_t p = r.path[r.points - 1];
      curve(&r, p.x, p.y, v, .15 / fmax(1, scale), 0);
      break;
    }
    case 8:
      if (!r.has_path)
        goto done;
      if (r.points)
        r.path[r.points - 1].close = 1;
      break;
    case 9: {
      for (unsigned i = 0; i < 4; i++)
        v[i] = number(s);
      if (!r.has_path || !s->valid || v[2] <= 0 || v[3] <= 0)
        goto done;
      double scale = fmax(hypot(state->matrix[0], state->matrix[1]),
                          hypot(state->matrix[2], state->matrix[3]));
      unsigned n = (unsigned)fmin(
          4096, fmax(16, ceil(PI * sqrt(fmax(v[2], v[3]) * scale / .15))));
      for (unsigned i = 0; i < n; i++) {
        double t = 2 * PI * i / n;
        point(&r, v[0] + v[2] * (.5 + .5 * cos(t)),
              v[1] + v[3] * (.5 + .5 * sin(t)), i == 0);
      }
      if (r.points)
        r.path[r.points - 1].close = 1;
      break;
    }
    case 10: {
      unsigned stroke = byte(s);
      int brush = (int16_t)word(s);
      double color[4];
      for (unsigned i = 0; i < 4; i++)
        color[i] = byte(s) / 255.;
      double line_width = number(s), opacity = number(s);
      if (!r.has_path || !s->valid || stroke > 1 || brush < -1 ||
          brush >= (int)count || line_width < 0 || opacity < 0 || opacity > 1)
        goto done;
      r.cache_record = 0;
      coverage_entry_t *cached = brush < 0 ? coverage_find(&r, state, stroke, line_width) : NULL;
      if (cached) {
        raster_cached(&r, state, color, opacity, cached);
        break;
      }
      make_edges(&r, state->matrix, (int)stroke, line_width);
      raster(&r, state, brush < 0 ? NULL : &gradients[brush], color, opacity,
             NULL);
      break;
    }
    case 11: {
      r.cache_record = 0;
      if (!r.has_path || r.clip_count >= 32)
        goto done;
      uint8_t *mask = grow(&r, NULL, 0, (size_t)width * height);
      if (!mask)
        goto done;
      r.clips[r.clip_count++] = mask;
      memset(mask, 0, (size_t)width * height);
      double color[] = {0, 0, 0, 1};
      make_edges(&r, state->matrix, 0, 0);
      raster(&r, state, NULL, color, 1, mask);
      state->clip = mask;
      break;
    }
    case 13: {
      unsigned n = word(s);
      if (!r.has_path || n < 3 || commands + n >= 65536)
        goto done;
      commands += n;
      for (unsigned i = 0; i < n; i++) {
        double x = (int16_t)word(s) / 4., y = (int16_t)word(s) / 4.;
        if (!s->valid)
          goto done;
        point(&r, x, y, i == 0);
      }
      if (r.points)
        r.path[r.points - 1].close = 1;
      break;
    }
    case 14: {
      unsigned n = word(s), unit = byte(s);
      if (version != 2 || !r.has_path || n < 3 || commands + n >= 65536 ||
          (unit != 1 && unit != 4))
        goto done;
      commands += n;
      int32_t x = 0, y = 0;
      for (unsigned i = 0; i < n; i++) {
        if (!s->valid ||
            !h2_lua_vector_delta_next(&s->p, s->end, unit, &x) ||
            !h2_lua_vector_delta_next(&s->p, s->end, unit, &y))
          goto done;
        point(&r, x / 4., y / 4., i == 0);
      }
      if (r.points)
        r.path[r.points - 1].close = 1;
      break;
    }
    default:
      goto done;
    }
  }
done:
  for (unsigned i = 0; i < r.clip_count; i++)
    free(r.clips[i]);
  free(r.path);
  for (unsigned i = 0; i < r.edge_capacity / EDGE_BLOCK; i++)
    free(r.edge_blocks[i]);
  free(r.crossings);
  free(r.active);
  free(r.unit_circle);
  free(r.coverage);
  free(gradients);
  free(states);
  return success && r.valid ? H2_LUA_VECTOR_SW_OK : r.failure;
}

h2_lua_vector_sw_result_t h2_lua_vector_sw_render_result(const uint8_t *data, size_t length,
    uint8_t *rgba, uint8_t *const *rows, unsigned width, unsigned height,
    const double matrix[6], h2_lua_vector_sw_poll_fn poll, void *user) {
  return h2_lua_vector_sw_render_cached_result(data, length, rgba, rows, width, height,
                                               matrix, poll, user, NULL);
}
int h2_lua_vector_sw_render_with_poll(const uint8_t *data, size_t length,
    uint8_t *rgba, unsigned width, unsigned height, const double matrix[6],
    h2_lua_vector_sw_poll_fn poll, void *user) {
  return h2_lua_vector_sw_render_result(data, length, rgba, NULL, width, height, matrix, poll, user)==H2_LUA_VECTOR_SW_OK;
}
int h2_lua_vector_sw_render_rows_with_poll(const uint8_t *data, size_t length,
    uint8_t *const *rows, unsigned width, unsigned height, const double matrix[6],
    h2_lua_vector_sw_poll_fn poll, void *user) {
  return h2_lua_vector_sw_render_result(data, length, NULL, rows, width, height, matrix, poll, user)==H2_LUA_VECTOR_SW_OK;
}

int h2_lua_vector_sw_render(const uint8_t *data, size_t length, uint8_t *rgba,
                            unsigned width, unsigned height,
                            const double matrix[6]) {
  return h2_lua_vector_sw_render_with_poll(data, length, rgba, width, height,
                                          matrix, NULL, NULL);
}
