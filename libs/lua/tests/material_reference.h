#ifndef H2_LUA_TEST_MATERIAL_REFERENCE_H
#define H2_LUA_TEST_MATERIAL_REFERENCE_H

/* Frozen 5e7a7a1 Q24 scan oracle. Keep the arithmetic unchanged when
 * optimizing production; only pixel writes use this independent test sink. */
typedef struct reference_quad_mapping {
  double first, last, ratio;
} reference_quad_mapping_t;

static double reference_quad_map_u(const reference_quad_mapping_t *mapping,
                                 double u) {
  if (u <= mapping->first) return 0;
  if (u >= mapping->last) return 1;
  double s = (u - mapping->first) / (mapping->last - mapping->first);
  if (mapping->ratio == 1) return s;
  /* Equivalent to s*r/(1+(r-1)*s), without overflow or cancellation of
   * the denominator for extreme positive ratios. Endpoints stay exact. */
  if (mapping->ratio >= 1)
    return s / (s + (1 - s) / mapping->ratio);
  volatile double scaled = s * mapping->ratio;
  return scaled / ((1 - s) + scaled);
}

#define DISPLAY_MATERIAL_KNOTS 32
#define DISPLAY_MATERIAL_CELLS 256
#define DISPLAY_MATERIAL_SCALE 16777216.0
#define DISPLAY_MATERIAL_UNIT INT64_C(16777216)
typedef struct reference_material {
  unsigned nu, nv;
  /* Smallest rectangle containing all nontransparent parameter cells. */
  unsigned u_first, u_end, v_first, v_end;
  size_t color_count;
  double u[DISPLAY_MATERIAL_KNOTS], v[DISPLAY_MATERIAL_KNOTS];
  uint16_t owner[]; /* zero is transparent; other values are palette index + 1 */
} reference_material_t;

typedef struct reference_material_edge {
  int64_t x, step;
  int axis, direction, horizontal, row_switch;
  unsigned boundary;
} reference_material_edge_t;
typedef struct reference_material_event { int x, edge; } reference_material_event_t;

static double reference_material_cross(double ax, double ay, double bx, double by) {
  return ax * by - ay * bx;
}

/* Setup only: double coordinates, followed by Q24 additions per scanline.
 * No per-pixel division, UV inversion, geometry expansion or allocation.
 * The bounded total magnitude prevents overflow including the final advance.
 */
static int reference_material_prepare(const reference_material_t *m, const double *c,
                            int first, int end, reference_material_edge_t *edges,
                            const reference_quad_mapping_t *mapping) {
  double orientation = 0;
  for (int i = 0; i < 4; ++i) {
    int j = (i + 1) % 4, k = (i + 2) % 4;
    double area = reference_material_cross(c[2*j]-c[2*i], c[2*j+1]-c[2*i+1],
                                c[2*k]-c[2*j], c[2*k+1]-c[2*j+1]);
    if (fabs(area) < 1e-8 || (i && area * orientation <= 0)) return 0;
    orientation = area;
  }
  for (int axis = 0; axis < 2; ++axis) {
    unsigned count = axis ? m->nv : m->nu;
    const double *knots = axis ? m->v : m->u;
    for (unsigned i = 0; i < count; ++i) {
      double t = knots[i];
      if (axis == 0 && mapping != NULL)
        t = reference_quad_map_u(mapping, t);
      int a = 0, b = axis ? 6 : 2, d = axis ? 2 : 6, e = 4;
      double px = c[a] + t * (c[b] - c[a]);
      double py = c[a+1] + t * (c[b+1] - c[a+1]);
      double qx = c[d] + t * (c[e] - c[d]);
      double qy = c[d+1] + t * (c[e+1] - c[d+1]);
      double sign = (orientation > 0 ? 1 : -1) * (axis ? 1 : -1);
      double nx = -(qy - py) * sign, ny = (qx - px) * sign;
      reference_material_edge_t *edge = &edges[(axis ? m->nu : 0) + i];
      edge->axis = axis;
      edge->boundary = i == 0 ? (axis ? 4u : 1u) :
                       i == count - 1 ? (axis ? 8u : 2u) : 0;
      edge->horizontal = nx == 0;
      if (edge->horizontal) {
        edge->direction = ny > 0 ? 1 : -1;
        edge->row_switch = ny > 0 ? (int)ceil(py) : (int)floor(py) + 1;
        edge->x = edge->step = 0;
      } else {
        double step = -ny / nx;
        double x = px - py * step;
        if (!isfinite(x) || !isfinite(step) ||
            fabs(x) + fabs(step) * (end + 1.0) > 1e9) return 0;
        edge->direction = nx > 0 ? 1 : -1;
        edge->x = (int64_t)llround(x * DISPLAY_MATERIAL_SCALE);
        edge->step = (int64_t)llround(step * DISPLAY_MATERIAL_SCALE);
        edge->x += edge->step * first;
      }
    }
  }
  return 1;
}

/* First integer X after a sign transition. Match the original Q24 sweep,
 * including negative coordinates and equality on decreasing boundaries. */
static int reference_material_threshold(const reference_material_edge_t *edge) {
  int64_t floor_x = edge->x / DISPLAY_MATERIAL_UNIT;
  int64_t remainder = edge->x % DISPLAY_MATERIAL_UNIT;
  if (remainder < 0) --floor_x;
  return (int)(floor_x + (edge->direction < 0 || remainder != 0));
}

static int reference_material_clip_row(const reference_material_t *m,
    const reference_material_edge_t *edges, int y, int *left, int *right) {
  const unsigned boundaries[8] = {0, m->nu - 1, m->nu, m->nu + m->nv - 1,
      m->u_first, m->u_end, m->nu + m->v_first, m->nu + m->v_end};
  for (unsigned i = 0; i < 8; ++i) {
    if (i >= 4 && boundaries[i] == boundaries[i - 4]) continue;
    const reference_material_edge_t *edge = &edges[boundaries[i]];
    int positive = !(i & 1u);
    if (edge->horizontal) {
      int sign = edge->direction > 0 ? y >= edge->row_switch : y < edge->row_switch;
      if (sign != positive) return 0;
    } else {
      int crossing = reference_material_threshold(edge);
      if ((edge->direction > 0) == positive) {
        if (crossing > *left) *left = crossing;
      } else if (crossing < *right) *right = crossing;
      if (*left >= *right) return 0;
    }
  }
  return 1;
}

/* Keep scan scratch out of the legacy fallback's call stack. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static int reference_raster_material(h2_lua_job_t *job,
    const reference_material_t *m, const uint16_t *colors,
    const double corners[8], int top, int bottom,
    const reference_quad_mapping_t *mapping) {
  if (m->u_first >= m->u_end || m->v_first >= m->v_end) return 1;
  reference_material_edge_t edges[2 * DISPLAY_MATERIAL_KNOTS];
  if (!reference_material_prepare(m, corners, top, bottom, edges, mapping)) return 0;
  unsigned count = m->nu + m->nv;
  int width = job->display_info.width;
  for (int y = top; y < bottom; ++y) {
    int row_left = 0, row_right = width;
    if (!reference_material_clip_row(m, edges, y, &row_left, &row_right)) {
      for (unsigned i = 0; i < count; ++i) edges[i].x += edges[i].step;
      continue;
    }
    /* Inside a convex bilinear patch, positive-side boundary counts give
     * the cell indices without inverting its rational parameter mapping.
     * Infinite grid lines can meet outside the patch, so also track the four
     * outer half-planes: u>=0, u<1, v>=0, v<1 is the bit pattern 0101. */
    int cell[2] = {-1, -1};
    unsigned mask = 0, used = 0;
    reference_material_event_t events[2 * DISPLAY_MATERIAL_KNOTS];
    for (unsigned i = 0; i < count; ++i) {
      reference_material_edge_t *e = &edges[i];
      int positive;
      if (e->horizontal) {
        positive = e->direction > 0 ? y >= e->row_switch : y < e->row_switch;
      } else {
        int crossing = reference_material_threshold(e);
        positive = e->direction > 0 ? crossing <= row_left : crossing > row_left;
        if (crossing > row_left && crossing < row_right) {
          unsigned at = used++;
          while (at && events[at-1].x > crossing) {
            events[at] = events[at-1]; --at;
          }
          events[at] = (reference_material_event_t){(int)crossing, (int)i};
        }
        e->x += e->step;
      }
      cell[e->axis] += positive;
      if (positive) mask |= e->boundary;
    }
    int left = row_left;
    for (unsigned i = 0; i <= used; ++i) {
      int right = i < used ? events[i].x : row_right;
      if (left < right && mask == 5u && cell[0] >= 0 && cell[1] >= 0 &&
          cell[0] < (int)m->nu-1 && cell[1] < (int)m->nv-1) {
        unsigned owner = m->owner[cell[1] * (m->nu - 1) + cell[0]];
        if (owner) {
          for (int x = left; x < right; ++x)
            ((uint16_t *)job->framebuffer)[y * width + x] = colors[owner-1];

        }
      }
      if (i < used) {
        reference_material_edge_t *e = &edges[events[i].edge];
        cell[e->axis] += e->direction;
        mask ^= e->boundary;
      }
      left = right;
    }
  }
  return 1;
}


#endif
