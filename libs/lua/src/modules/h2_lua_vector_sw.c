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
  double offset, color[4];
} stop_t;
typedef struct {
  unsigned radial, count;
  double xy[4];
  stop_t stops[16];
} gradient_t;
typedef struct {
  double matrix[6];
  uint8_t *clip;
} state_t;
typedef struct {
  unsigned width, height, points, point_capacity, edges, edge_capacity;
  size_t allocated;
  int valid, has_path;
  point_t *path;
  edge_t *edge;
  crossing_t *crossings;
  unsigned crossing_capacity;
  double *coverage;
  uint8_t *rgba, *clips[32];
  unsigned clip_count;
  double bounds[4];
} render_t;

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
    r->valid = 0;
    return NULL;
  }
  void *q = realloc(p, new_size);
  if (!q) {
    r->valid = 0;
    return NULL;
  }
  r->allocated += new_size - old_size;
  return q;
}
static void point(render_t *r, double x, double y, int move) {
  if (!r->valid)
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
static void edge(render_t *r, const double m[6], double x0, double y0,
                 double x1, double y1) {
  if (!r->valid)
    return;
  double a = m[0] * x0 + m[2] * y0 + m[4], b = m[1] * x0 + m[3] * y0 + m[5];
  double c = m[0] * x1 + m[2] * y1 + m[4], d = m[1] * x1 + m[3] * y1 + m[5];
  if (fabs(d - b) < 1e-10)
    return;
  if (r->edges == r->edge_capacity) {
    unsigned n = r->edge_capacity ? r->edge_capacity * 2 : 256;
    if (n > MAX_EDGES) {
      r->valid = 0;
      return;
    }
    void *p =
        grow(r, r->edge, r->edge_capacity * sizeof(edge_t), n * sizeof(edge_t));
    if (!p)
      return;
    r->edge = p;
    r->edge_capacity = n;
  }
  r->edge[r->edges++] = (edge_t){(float)a, (float)b, (float)c, (float)d};
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
  double x = p.x + radius, y = p.y;
  for (unsigned i = 1; i <= n; i++) {
    double t = 2 * PI * i / n, nx = p.x + radius * cos(t),
           ny = p.y + radius * sin(t);
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
static int compare_crossing(const void *a, const void *b) {
  float x = ((const crossing_t *)a)->x, y = ((const crossing_t *)b)->x;
  return (x > y) - (x < y);
}
static void cover(render_t *r, double left, double right) {
  left = fmax(0, left);
  right = fmin(r->width, right);
  if (right <= left)
    return;
  unsigned a = (unsigned)floor(left), b = (unsigned)ceil(right);
  for (unsigned x = a; x < b; x++)
    r->coverage[x] += (fmin(right, x + 1.) - fmax(left, x)) * .25;
}
static void gradient_color(const gradient_t *g, double x, double y,
                           double out[4]) {
  double t;
  if (g->radial)
    t = hypot(x - g->xy[0], y - g->xy[1]) / g->xy[2];
  else {
    double dx = g->xy[2] - g->xy[0], dy = g->xy[3] - g->xy[1],
           d = dx * dx + dy * dy;
    t = d > 1e-20 ? ((x - g->xy[0]) * dx + (y - g->xy[1]) * dy) / d : 0;
  }
  unsigned j = 0;
  while (j + 1 < g->count && t > g->stops[j + 1].offset)
    j++;
  const stop_t *a = &g->stops[j], *b = &g->stops[j + 1 < g->count ? j + 1 : j];
  double d = b->offset - a->offset,
         u = d > 0 ? fmin(1, fmax(0, (t - a->offset) / d)) : 0;
  for (unsigned k = 0; k < 4; k++)
    out[k] = a->color[k] * (1 - u) + b->color[k] * u;
}
static void raster(render_t *r, const state_t *state, const gradient_t *g,
                   const double color[4], double opacity, uint8_t *mask) {
  if (!r->valid || !r->edges)
    return;
  if (r->crossing_capacity < r->edges) {
    void *p = grow(r, r->crossings, r->crossing_capacity * sizeof(crossing_t),
                   r->edges * sizeof(crossing_t));
    if (!p)
      return;
    r->crossings = p;
    r->crossing_capacity = r->edges;
  }
  const double *m = state->matrix;
  double determinant = m[0] * m[3] - m[1] * m[2];
  double bw = r->bounds[2] - r->bounds[0], bh = r->bounds[3] - r->bounds[1];
  double ymin = r->height, ymax = 0;
  for (unsigned i = 0; i < r->edges; i++) {
    ymin = fmin(ymin, fmin(r->edge[i].y0, r->edge[i].y1));
    ymax = fmax(ymax, fmax(r->edge[i].y0, r->edge[i].y1));
  }
  unsigned start = (unsigned)fmax(0, fmin(r->height, floor(ymin)));
  unsigned end = (unsigned)fmax(0, fmin(r->height, ceil(ymax)));
  for (unsigned y = start; y < end; y++) {
    memset(r->coverage, 0, r->width * sizeof(double));
    for (unsigned sample = 0; sample < 4; sample++) {
      double sy = y + (sample + .5) * .25;
      unsigned n = 0;
      for (unsigned i = 0; i < r->edges; i++) {
        edge_t e = r->edge[i];
        if (sy >= fmin(e.y0, e.y1) && sy < fmax(e.y0, e.y1))
          r->crossings[n++] = (crossing_t){
              (float)(e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0)),
              e.y1 > e.y0 ? 1 : -1};
      }
      qsort(r->crossings, n, sizeof(crossing_t), compare_crossing);
      int winding = 0;
      double left = 0;
      for (unsigned i = 0; i < n; i++) {
        if (winding)
          cover(r, left, r->crossings[i].x);
        winding += r->crossings[i].winding;
        left = r->crossings[i].x;
      }
    }
    for (unsigned x = 0; x < r->width; x++) {
      size_t p = (size_t)y * r->width + x;
      double coverage = fmin(1, r->coverage[x]);
      if (state->clip)
        coverage *= state->clip[p] / 255.;
      if (mask) {
        mask[p] = (uint8_t)floor(coverage * 255 + .5);
        continue;
      }
      if (coverage <= 0)
        continue;
      double c[4];
      memcpy(c, color, sizeof(c));
      if (g) {
        if (bw <= 0 || bh <= 0)
          continue;
        double dx = x + .5 - m[4], dy = y + .5 - m[5];
        double ox = (m[3] * dx - m[2] * dy) / determinant,
               oy = (-m[1] * dx + m[0] * dy) / determinant;
        gradient_color(g, (ox - r->bounds[0]) / bw, (oy - r->bounds[1]) / bh,
                       c);
      }
      double a = c[3] * opacity * coverage;
      for (unsigned k = 0; k < 3; k++)
        r->rgba[p * 4 + k] = (uint8_t)fmin(
            255, floor(c[k] * 255 * a + r->rgba[p * 4 + k] * (1 - a) + .5));
      r->rgba[p * 4 + 3] = (uint8_t)fmin(
          255, floor(255 * a + r->rgba[p * 4 + 3] * (1 - a) + .5));
    }
  }
}

int h2_lua_vector_sw_render(const uint8_t *data, size_t length, uint8_t *rgba,
                            unsigned width, unsigned height,
                            const double matrix[6]) {
  if (!data || !rgba || !matrix || length < 12 || length > 2000000 ||
      memcmp(data, "H2VG", 4) || !width || !height || width > 4096 ||
      height > 4096)
    return 0;
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
  render_t r = {.width = width, .height = height, .rgba = rgba, .valid = 1};
  r.coverage = calloc(width, sizeof(double));
  int success = 0;
  unsigned depth = 0, commands = 0;
  if (!gradients || !states || !r.coverage)
    goto done;
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
  memset(rgba, 0, (size_t)width * height * 4);
  while (s->valid && r.valid && s->p < s->end && ++commands < 65536) {
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
      make_edges(&r, state->matrix, (int)stroke, line_width);
      raster(&r, state, brush < 0 ? NULL : &gradients[brush], color, opacity,
             NULL);
      break;
    }
    case 11: {
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
  free(r.edge);
  free(r.crossings);
  free(r.coverage);
  free(gradients);
  free(states);
  return success && r.valid;
}
