/* Orthographic room solids. The height of every visible sample is cached with
 * the static room and also clips the walker's billboard against counter lips.
 */
#include "buffet_mesh.h"
#include "buffet_lighting.h"
#include "buffet_rotary.h"
#include "buffet_scale.h"
#include <math.h>
#include <string.h>

enum { W = BF_FRAME_W, H = BF_FRAME_H, CORNER = 8, OUTLINE = 4 * (CORNER + 1) };
static const float tau = 6.28318530718f;
typedef struct {
  float x, y, z;
} vertex_t;
typedef struct {
  bf_canvas_t canvas;
  bool cache;
} mesh_t;
static uint16_t pigment(unsigned rgb, float light) {
  int r = (int)fminf(255, (rgb >> 16) * light),
      g = (int)fminf(255, ((rgb >> 8) & 255) * light),
      b = (int)fminf(255, (rgb & 255) * light);
  return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
static vertex_t vertex(bf_world_t p, float z) {
  bf_point_t screen = bf_project(p, z);
  return (vertex_t){screen.x * BF_SCALE_NUM / BF_SCALE_DEN,
                    screen.y * BF_SCALE_NUM / BF_SCALE_DEN, z * 2 + 40};
}
/* Scan convex faces at pixel centers. Interpolating world height gives a
 * common depth order for steps, vessels, walls, belts and people. */
static void face(mesh_t m, const vertex_t* p, int n, uint16_t color) {
  float ymin = p[0].y, ymax = ymin;
  for (int i = 1; i < n; i++) {
    ymin = fminf(ymin, p[i].y);
    ymax = fmaxf(ymax, p[i].y);
  }
  int y0 = (int)fmaxf(0, ceilf(ymin - .5f)),
      y1 = (int)fminf(H, ceilf(ymax - .5f));
  for (int y = y0; y < y1; y++) {
    float row = y + .5f, left = 1e9f, right = -1e9f, zl = 0, zr = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      const vertex_t a = p[j], b = p[i];
      if ((a.y > row) == (b.y > row)) continue;
      float t = (row - a.y) / (b.y - a.y), x = a.x + t * (b.x - a.x),
            z = a.z + t * (b.z - a.z);
      if (x < left) {
        left = x;
        zl = z;
      }
      if (x > right) {
        right = x;
        zr = z;
      }
    }
    if (left >= right) continue;
    int start = (int)fmaxf(0, ceilf(left - .5f)),
        end = (int)fminf(W, ceilf(right - .5f));
    float dz = (zr - zl) / (right - left), z = zl + (start + .5f - left) * dz;
    for (int x = start; x < end; x++, z += dz) {
      int i = y * W + x;
      uint8_t height = (uint8_t)fminf(255, fmaxf(0, z + .5f));
      if (height < m.canvas.heights[i]) continue;
      m.canvas.pixels[i] = color;
      if (m.cache) m.canvas.heights[i] = height;
    }
  }
}
static bf_footprint_t rect(float u, float v, float w, float h, float r) {
  return (bf_footprint_t){u, v, w, h, r};
}
static void outline(bf_footprint_t r, bf_world_t* pts) {
  float radius = fminf(r.r, fminf(r.w, r.h) * .5f);
  int j = 0;
  for (int k = 0; k < 4; k++) {
    float u = r.u + ((k == 0 || k == 1) ? r.w - radius : radius),
          v = r.v + (k == 1 || k == 2 ? r.h - radius : radius);
    for (int i = 0; i <= CORNER; i++) {
      float a = (-1 + k + i / (float)CORNER) * tau * .25f;
      pts[j++] = (bf_world_t){u + cosf(a) * radius, v + sinf(a) * radius};
    }
  }
}
static void roof(mesh_t m, bf_footprint_t r, float z, unsigned rgb) {
  bf_world_t p[OUTLINE];
  vertex_t q[OUTLINE];
  outline(r, p);
  for (int i = 0; i < OUTLINE; i++) q[i] = vertex(p[i], z);
  face(m, q, OUTLINE, pigment(rgb, 1));
}
static void panel4(mesh_t m, bf_world_t a, bf_world_t b, bf_world_t c,
                   bf_world_t d, float z, unsigned rgb) {
  vertex_t v[4] = {vertex(a, z), vertex(b, z), vertex(c, z), vertex(d, z)};
  face(m, v, 4, pigment(rgb, 1));
}
static void strip(mesh_t m, bf_world_t a, bf_world_t b, float width, float z,
                  unsigned rgb) {
  float u = b.u - a.u, v = b.v - a.v, len = hypotf(u, v);
  if (!len) return;
  float su = -v * width / (2 * len), sv = u * width / (2 * len);
  panel4(m, (bf_world_t){a.u + su, a.v + sv}, (bf_world_t){b.u + su, b.v + sv},
         (bf_world_t){b.u - su, b.v - sv}, (bf_world_t){a.u - su, a.v - sv}, z,
         rgb);
}
static void skirt(mesh_t m, const bf_world_t* a, const bf_world_t* b, int n,
                  float low, float high, unsigned rgb) {
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float du = b[i].u - b[j].u, dv = b[i].v - b[j].v, len = hypotf(du, dv);
    if (len < .0001f) continue;
    float nx = dv / len, ny = -du / len;
    float sheen = fmaxf(0, -.88f * nx - .47f * ny);
    float shade =
        .84f - .22f * nx + .12f * ny + .15f * sheen * sheen * sheen * sheen;
    vertex_t q[4] = {vertex(a[j], low), vertex(a[i], low), vertex(b[i], high),
                     vertex(b[j], high)};
    face(m, q, 4, pigment(rgb, shade));
  }
}
static void bevel(mesh_t m, bf_footprint_t r, float low, float top, float edge,
                  unsigned side, unsigned cap, unsigned rim) {
  bf_world_t a[OUTLINE], b[OUTLINE];
  outline(r, a);
  outline(bf_inset(r, fminf(edge * .020f, fminf(r.w, r.h) * .16f)), b);
  skirt(m, a, a, OUTLINE, low, top - edge, side);
  skirt(m, a, b, OUTLINE, top - edge, top, rim);
  vertex_t p[OUTLINE];
  for (int i = 0; i < OUTLINE; i++) p[i] = vertex(b[i], top);
  face(m, p, OUTLINE, pigment(cap, 1));
}
static void vessel_band(mesh_t m, bf_world_t center, float bottom, float top,
                        float r0, float r1, unsigned color, unsigned lid) {
  enum { N = BF_FRAME_W < 800 ? 14 : 28 };
  bf_world_t lower[N], upper[N];
  vertex_t q[N];
  for (int i = 0; i < N; i++) {
    float a = i * tau / N, u = cosf(a), v = sinf(a);
    lower[i] = (bf_world_t){center.u + u * r0, center.v + v * r0};
    upper[i] = (bf_world_t){center.u + u * r1, center.v + v * r1};
    q[i] = vertex(upper[i], top);
  }
  skirt(m, lower, upper, N, bottom, top, color);
  if (lid) face(m, q, N, pigment(lid, 1));
}
static void dish_stack(mesh_t m, float u, float v, float z, int count) {
  bf_world_t p = {u, v};
  vessel_band(m, p, z, z + 1.5f, .19f, .21f, 0x748c83, 0);
  z += 1.5f;
  for (int i = 0; i < count; i++) {
    vessel_band(m, p, z, z + 1.2f, .29f, .42f, 0x68908a, 0);
    vessel_band(m, p, z + 1.2f, z + 1.9f, .42f, .425f, 0xeff0dc, 0xe7e6cf);
    z += 2.1f;
  }
  vessel_band(m, p, z, z + .2f, .34f, .34f, 0x99b2a3, 0xbdcbb8);
  vessel_band(m, p, z + .2f, z + .35f, .22f, .22f, 0x7a9b94, 0xd3deca);
}
static void cups(mesh_t m, float u, float v, float z) {
  for (int i = 0; i < 2; i++) {
    bf_world_t p = {u + i * .40f, v};
    vessel_band(m, p, z, z + 5, .13f, .16f, 0x57786e, 0xe3d7b7);
    vessel_band(m, p, z + 5, z + 5.3f, .115f, .115f, 0x71543a, 0x71543a);
  }
}
/* Shadows lie on a single horizontal plane. Tint the tile colour while
 * retaining the height that movers use for occlusion. */
static void cast_shadow(mesh_t m, bf_footprint_t r, float z, unsigned rgb,
                        unsigned alpha) {
  bf_world_t contour[OUTLINE];
  vertex_t projected[OUTLINE];
  float min_y = H, max_y = 0;
  outline(r, contour);
  for (int i = 0; i < OUTLINE; i++) {
    projected[i] = vertex(contour[i], z);
    min_y = fminf(min_y, projected[i].y);
    max_y = fmaxf(max_y, projected[i].y);
  }
  int first = (int)fmaxf(0, ceilf(min_y - .5f));
  int end = (int)fminf(H, ceilf(max_y - .5f));
  unsigned base = 255 - alpha;
  uint16_t dye = pigment(rgb, 1);
  unsigned rr = (dye >> 11) * alpha + 127;
  unsigned gg = ((dye >> 5) & 63) * alpha + 127;
  unsigned bb = (dye & 31) * alpha + 127;
  uint8_t plane = (uint8_t)(projected[0].z + .5f);
  for (int y = first; y < end; y++) {
    float row = y + .5f, left = W, right = 0;
    for (int i = 0, j = OUTLINE - 1; i < OUTLINE; j = i++) {
      vertex_t a = projected[j], b = projected[i];
      if ((a.y > row) == (b.y > row)) continue;
      float x = a.x + (row - a.y) * (b.x - a.x) / (b.y - a.y);
      left = fminf(left, x);
      right = fmaxf(right, x);
    }
    int start_x = (int)fmaxf(0, ceilf(left - .5f));
    int end_x = (int)fminf(W, ceilf(right - .5f));
    for (int x = start_x; x < end_x; x++) {
      int i = y * W + x;
      if (m.canvas.heights[i] > plane) continue;
      uint16_t c = m.canvas.pixels[i];
      unsigned red = ((c >> 11) * base + rr) / 255;
      unsigned green = (((c >> 5) & 63) * base + gg) / 255;
      unsigned blue = ((c & 31) * base + bb) / 255;
      m.canvas.pixels[i] = (uint16_t)((red << 11) | (green << 5) | blue);
    }
  }
}

static void shadow(mesh_t m, bf_footprint_t r, float z, unsigned color) {
  bf_footprint_t penumbra = r;
  penumbra.u += .17f;
  penumbra.v += .30f;
  cast_shadow(m, bf_inset(penumbra, -.12f), z + .16f, 0x5d5960, 48);
  r.u += .12f;
  r.v += .19f;
  cast_shadow(m, bf_inset(r, -.055f), z + .18f, 0x625865, 88);
  r.u -= .08f;
  r.v -= .09f;
  cast_shadow(m, r, z + .20f, color, 120);
}
static void floor_and_steps(mesh_t m) {
  /* A small ordered dither keeps the dusk backdrop quiet in RGB565. */
  static const int dither[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float k = (float)y / H, n = dither[y % 4][x % 4] / 16.f - .5f;
      unsigned red = (unsigned)(29 + 10 * k + 7 * n),
               green = (unsigned)(55 + 16 * k + 3 * n),
               blue = (unsigned)(58 + 14 * k + 7 * n);
      m.canvas.pixels[y * W + x] =
          (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3));
    }
  bf_footprint_t ground = rect(-.16f, -.12f, 20.32f, 13.28f, .54f);
  bevel(m, ground, -14, 0, 1.8f, 0x6f5744, 0xd0c6ad, 0xffdfae);
  bevel(m, bf_inset(ground, .06f), -12, -9, 1, 0x553d35, 0x96704d, 0xdbac74);
  static const unsigned tiles[] = {0xefe0c6, 0xe3d2b7, 0xeee3cb, 0xe6d8c0,
                                   0xedded0};
  for (int v = 0; v < BF_ROWS; v++)
    for (int u = 0; u < BF_COLS; u++)
      roof(m, rect(u + .017f, v + .017f, .966f, .966f, .033f), .32f,
           tiles[(u * 13 + v * 7 + u * v) % 5]);
  bf_footprint_t terrace = rect(.04f, .05f, 19.92f, 2.50f, .38f);
  shadow(m, terrace, .34f, 0xbeb193);
  bevel(m, terrace, .35f, 8, 1.5f, 0x9d7858, 0xc1b28e, 0xf3dfb0);
  /* Three treads overlap vertically, so the raised rear aisle is accessible
   * through the same stair interval used by navigation. */
  for (int i = 0; i < 3; i++) {
    float y = 2.40f + i * .267f;
    bevel(m, rect(8.49f, y, 3.02f, .42f, .16f), .3f, 7.5f - i * 2.5f, 1.0f,
          0xad8457, 0xe6ca98, 0xfae2b3);
  }
  for (int i = 0; i < 20; i++)
    strip(m, (bf_world_t){i + .02f, 1.48f}, (bf_world_t){i + .02f, 2.43f},
          .015f, 8.25f, 0xac9b7c);
  roof(m, rect(8.16f, 11.60f, 3.68f, 1.17f, .14f), .8f, 0x536f66);
  roof(m, rect(8.25f, 11.69f, 3.5f, .99f, .11f), 1.1f, 0x607d70);
  for (int j = 0; j < 3; j++)
    strip(m, (bf_world_t){8.48f, 11.87f + j * .26f},
          (bf_world_t){11.52f, 11.87f + j * .26f}, .033f, 1.3f, 0xa8b397);
}
static void back_wall(mesh_t m) {
  bevel(m, rect(-.02f, -.20f, 20.02f, .45f, .21f), 2, 47, 1.7f, 0xb39d88,
        0x79593d, 0xd7bc96);
  bevel(m, rect(-.04f, -.20f, 20.08f, .50f, .21f), 3, 8.5f, 1, 0x655a4d,
        0xa48763, 0xc39c6c);
  bevel(m, rect(-.07f, -.26f, 20.14f, .66f, .24f), 43, 48, 1.5f, 0x674f43,
        0xa67751, 0xe2b17c);
  bevel(m, rect(-.11f, -.30f, 20.22f, .76f, .25f), 48, 51.5f, 1.25f, 0x795039,
        0xb2865d, 0xeec899);
  const float posts[] = {.13f, 4.71f, 7.37f, 12.69f, 15.11f, 19.74f};
  for (unsigned i = 0; i < sizeof(posts) / sizeof(posts[0]); i++) {
    float u = posts[i];
    bevel(m, rect(u - .11f, -.15f, .22f, .69f, .065f), 4, 53.0f, 1.15f,
          0x65584e, 0x9a8167, 0xae9473);
    bevel(m, rect(u - .16f, -.19f, .32f, .77f, .07f), 46, 49, .8f, 0x675248,
          0xa3876b, 0xc4a782);
    bevel(m, rect(u - .14f, -.15f, .28f, .70f, .055f), 4, 12, .7f, 0x474a46,
          0x81725f, 0x99846a);
  }
  /* Translucent shoji panels and their individual timber mullions. */
  for (int j = 0; j < 4; j += 3) {
    float u = .54f + j * 4.96f;
    bevel(m, rect(u - .36f, .32f, 4.44f, .30f, .07f), 26, 49, .85f, 0x72665b,
          0x977c5c, 0xc5a37f);
    bevel(m, rect(u - .25f, .59f, 4.20f, .13f, .05f), 28.5f, 47.5f, .55f,
          0xb1c8b3, 0xebdcb3, 0xe3cba8);
    bevel(m, rect(u - .28f, .71f, 4.24f, .12f, .045f), 34.4f, 35.8f, .45f,
          0x9d8368, 0xc5a37a, 0xe2c699);
    for (int k = 0; k < 6; k++)
      bevel(m, rect(u - .22f + k * .81f, .715f, .05f, .10f, .02f), 28.6f, 47.6f,
            .35f, 0x967d65, 0xc3a67f, 0xdfc49b);
    bevel(m, rect(u - .38f, .36f, 4.46f, .48f, .07f), 25.2f, 27.3f, .6f,
          0x83614c, 0xcbab7d, 0xe0c598);
  }
  for (int side = 0; side < 2; side++) {
    float u = side ? 19.78f : -.10f;
    bevel(m, rect(u, .18f, .33f, 10.45f, .15f), 0, 17, 1.2f, 0x88674f, 0xc3996d,
          0xe8c899);
    for (int j = 0; j < 3; j++) {
      float v = 1.55f + 3.02f * j;
      bevel(m, rect(u + .046f, v, .21f, 2.83f, .08f), 16.3f, 24.4f, .7f,
            0x586d65, 0x8c9c83, 0xb8be9d);
      bevel(m, rect(u + .027f, v - .05f, .26f, 2.92f, .09f), 24.4f, 26.0f, .55f,
            0x8b694e, 0xc79a6f, 0xe6c79c);
    }
    for (int j = 0; j < 4; j++) {
      float v = 1.44f + 3.02f * j;
      bevel(m, rect(u - .055f, v - .09f, .42f, .22f, .075f), 0, 28.4f, .8f,
            0x785d49, 0xb38b66, 0xd2ac83);
      bevel(m, rect(u - .066f, v - .102f, .44f, .24f, .075f), 25.6f, 28.8f, .8f,
            0x8f6b4c, 0xb38d5e, 0xe1ba87);
    }
  }
}
/* A stencil on a vertical timber or ceramic face shares the room depth. */
static void upright(mesh_t m, float u, float v, float width, float low,
                    float high, unsigned rgb) {
  bf_world_t a = {u, v}, b = {u + width, v};
  vertex_t quad[] = {vertex(a, low), vertex(b, low), vertex(b, high),
                     vertex(a, high)};
  face(m, quad, 4, pigment(rgb, 1));
}

static void pantry_bowl(mesh_t m, float u, float v, float z, float radius,
                        unsigned glaze) {
  bf_world_t center = {u, v};
  vessel_band(m, center, z, z + 1.4f, radius * .48f, radius * .52f, 0x728a85,
              0);
  vessel_band(m, center, z + 1.4f, z + 4.5f, radius * .54f, radius * .86f,
              glaze, 0);
  vessel_band(m, center, z + 4.5f, z + 6.5f, radius * .86f, radius, glaze, 0);
  vessel_band(m, center, z + 6.5f, z + 7.5f, radius, radius, 0xe7dabb,
              0xe4d9ba);
  vessel_band(m, center, z + 7.5f, z + 7.8f, radius * .82f, radius * .82f,
              0x80979a, 0x819a99);
  vessel_band(m, center, z + 7.8f, z + 8.0f, radius * .58f, radius * .58f,
              0xb4c2b2, 0xb4c2b2);
}

static void rice_tub(mesh_t m, float u, float v, float z) {
  bf_world_t center = {u, v};
  vessel_band(m, center, z, z + 9.8f, .38f, .50f, 0xcda36c, 0xdec18b);
  vessel_band(m, center, z + 2, z + 3.6f, .417f, .434f, 0x797762, 0);
  vessel_band(m, center, z + 7.0f, z + 8.6f, .479f, .501f, 0x777360, 0);
  vessel_band(m, center, z + 9.8f, z + 10.8f, .50f, .49f, 0xf0d79e, 0xf4d79e);
  strip(m, (bf_world_t){u - .41f, v + .01f}, (bf_world_t){u + .38f, v - .17f},
        .13f, z + 11.2f, 0xac8457);
}

static void pass_sign(mesh_t m) {
  float u = 8.32f, v = .75f;
  for (int i = 0; i < 2; i++)
    bevel(m, rect(u + .22f + 2.69f * i, v - .08f, .071f, .16f, .03f), 47, 66,
          .35f, 0x6a5143, 0xb68e58, 0xdbad75);
  /* A suspended oval-ended enamel sign with a hand-set tide and rice mark. */
  bevel(m, rect(u, v - .07f, 3.25f, .23f, .10f), 44.5f, 62.5f, 1.0f, 0x825f48,
        0xbc9468, 0xf4d8a0);
  upright(m, u + .08f, v + .17f, 3.09f, 45.9f, 61.5f, 0x275458);
  upright(m, u + .16f, v + .18f, 2.93f, 47.2f, 47.6f, 0xa5a98a);
  const float wave[][2] = {{.38f, 52},     {.61f, 50.3f},  {.88f, 51.4f},
                           {1.14f, 55.2f}, {1.39f, 57.5f}, {1.66f, 58.0f},
                           {1.81f, 56.9f}, {1.72f, 55.2f}, {1.50f, 55.1f}};
  for (unsigned i = 1; i < sizeof(wave) / sizeof(wave[0]); i++) {
    bf_world_t a = {u + wave[i - 1][0], v + .20f};
    bf_world_t b = {u + wave[i][0], v + .20f};
    vertex_t q[] = {vertex(a, wave[i - 1][1] - .7f),
                    vertex(b, wave[i][1] - .7f), vertex(b, wave[i][1] + .7f),
                    vertex(a, wave[i - 1][1] + .7f)};
    face(m, q, 4, pigment(0xffe4b8, 1));
  }
  vertex_t fish[] = {vertex((bf_world_t){u + 2.02f, v + .20f}, 53.5f),
                     vertex((bf_world_t){u + 2.27f, v + .20f}, 56.2f),
                     vertex((bf_world_t){u + 2.53f, v + .20f}, 56.1f),
                     vertex((bf_world_t){u + 2.73f, v + .20f}, 54.0f),
                     vertex((bf_world_t){u + 2.47f, v + .20f}, 51.3f),
                     vertex((bf_world_t){u + 2.22f, v + .20f}, 51.4f)};
  face(m, fish, 6, pigment(0xf2af78, 1));
}

static void pass_shelves(mesh_t m) {
  /* The shelves and canopy occupy the rear wall; the row of finished plates
   * has the full depth of the existing service pass in front of them. */
  upright(m, 7.39f, .39f, 5.35f, 27.0f, 55.f, 0x554d48);
  upright(m, 7.54f, .40f, 5.05f, 28.2f, 51.f, 0x746453);
  for (int j = 0; j < 3; j++) {
    float u = 7.34f + 2.67f * j;
    bevel(m, rect(u, -.10f, .18f, .58f, .07f), 25.5f, 58.4f, .8f, 0x6d4b3b,
          0xb78b60, 0xd4ae79);
  }
  bevel(m, rect(7.29f, .41f, 5.65f, .50f, .10f), 49.8f, 52.8f, 1, 0x674c40,
        0xb28c62, 0xdbb885);
  bevel(m, rect(7.44f, .30f, 5.33f, .26f, .08f), 31.1f, 33.8f, .65f, 0x7d5944,
        0xc69262, 0xe6bb8c);
  pantry_bowl(m, 7.96f, .41f, 33.9f, .28f, 0x8bb4ac);
  pantry_bowl(m, 8.57f, .41f, 34, .25f, 0xccc4a2);
  dish_stack(m, 11.53f, .36f, 34, 3);
  cups(m, 10.48f, .40f, 34);
  pass_sign(m);
}

static void cabinet(mesh_t m, int row) {
  bf_footprint_t r = bf_counters[row];
  bool along = row == 0;
  float z = along ? 8.0f : 0.0f;
  int modules = along ? 13 : 6;
  float pitch = (along ? r.w : r.h) / modules;
  shadow(m, r, z + .35f, 0xaa9579);
  bevel(m, bf_inset(r, .09f), z + 1, z + 4, 1, 0x6a4e3c, 0x986e4b, 0xa97b50);
  bevel(m, bf_inset(r, .035f), z + 3, z + 15.8f, 1, 0xb58359, 0xc49b70,
        0xdbb287);
  bevel(m, r, z + 15, z + 18, 1.25f, 0x956740, 0xe7c397, 0xffdfb4);
  for (int i = 0; i < modules; i++) {
    float start = (along ? r.u : r.v) + i * pitch;
    if (i) {
      bf_world_t a = along ? (bf_world_t){start, r.v + .1f}
                           : (bf_world_t){r.u + .1f, start};
      bf_world_t b = along ? (bf_world_t){start, r.v + r.h - .08f}
                           : (bf_world_t){r.u + r.w - .1f, start};
      strip(m, a, b, .022f, z + 18.3f, 0xc2976b);
    }
    /* Recessed frame and wooden door face sit in the outward-facing apron. */
    if (along) {
      float v = r.v + r.h - .016f;
      bevel(m, rect(start + .12f, v, pitch - .24f, .10f, .048f), z + 4.5f,
            z + 13.3f, .55f, 0x936941, 0xbd9870, 0xdbb78c);
      bevel(m, rect(start + .19f, v + .072f, pitch - .38f, .055f, .022f),
            z + 5.8f, z + 12.2f, .45f, 0xac7e54, 0xcfac81, 0xd8ac80);
      bevel(m, rect(start + pitch * .45f, v + .12f, .18f, .07f, .03f), z + 9.5f,
            z + 11.1f, .5f, 0x67553d, 0xb49463, 0xd2af78);
    } else if (i == 0) {
      dish_stack(m, r.u + r.w * .5f, start + pitch * .48f, z + 18.4f, 4);
    } else if (i <= BF_SIDE_SLOTS) {
      bf_footprint_t board =
          rect(r.u + .13f, start + .13f, r.w - .26f, pitch - .26f, .17f);
      roof(m, board, 18.38f, row == 1 ? 0xac806a : 0x778596);
      roof(m, bf_inset(board, .044f), 18.52f, row == 1 ? 0xd5a48b : 0xaeb5c3);
      if (BF_FRAME_W >= 800) {
        float u0 = board.u + .12f, u1 = board.u + board.w - .12f;
        strip(m, (bf_world_t){u0, board.v + .14f},
              (bf_world_t){u1, board.v + .14f}, .022f, 18.65f,
              row == 1 ? 0xe9c5a1 : 0xcbd1d2);
      }
    }
  }
  if (!along) return;
  for (int i = 0; i < 3; i++) {
    bf_footprint_t board =
        rect(8.5f + 1.5f * i - .54f, .38f, 1.08f, .91f, .11f);
    roof(m, board, 26.4f, 0x627e72);
    roof(m, bf_inset(board, .04f), 26.55f, 0x9db59b);
  }
  dish_stack(m, 2.48f, .84f, 26.4f, 5);
  dish_stack(m, 3.21f, .84f, 26.4f, 3);
  cups(m, 5.10f, .85f, 26.4f);
  dish_stack(m, 15.23f, .84f, 26.4f, 4);
  cups(m, 16.42f, .83f, 26.4f);
}
static void noren(mesh_t m, float u, unsigned cloth) {
  const float v = .86f;
  const unsigned hem = 0xf1d6a0;
  bevel(m, rect(u - .1f, v - .11f, 3.96f, .14f, .06f), 46, 48, .7f, 0x6a503c,
        0xcda878, 0xeac594);
  /* Folded cloth has a warm selvedge and cooler folds next to the posts. */
  for (int j = 0; j < 4; j++) {
    float x = u + j * .96f, z = 32.8f + ((j == 0 || j == 3) ? 1.0f : 0);
    unsigned cream = j % 2 ? 0xf6ddb3 : 0xd9c39d;
    bevel(m, rect(x, v, .91f, .12f, .055f), z, 47.2f, .65f, cloth, cloth,
          cloth);
    upright(m, x + .07f, v + .13f, .083f, z + 1.2f, 46.25f,
            j % 2 ? 0x956e59 : 0x566d68);
    upright(m, x + .20f, v + .132f, .41f, z + 2.1f, z + 2.8f, cream);
    bevel(m, rect(x + .04f, v + .094f, .83f, .026f, .012f), z + 1.7f, z + 2.25f,
          .1f, hem, hem, hem);
    /* A small stitched rise and fall repeats across the cloth. */
    bf_world_t a = {x + .20f, v + .12f}, b = {x + .44f, v + .12f},
               c = {x + .69f, v + .12f};
    vertex_t l[4] = {vertex(a, 38.7f), vertex(b, 40.7f), vertex(b, 41.9f),
                     vertex(a, 39.9f)};
    vertex_t r[4] = {vertex(b, 40.7f), vertex(c, 38.7f), vertex(c, 39.9f),
                     vertex(b, 41.9f)};
    face(m, l, 4, pigment(hem, 1));
    face(m, r, 4, pigment(hem, 1));
  }
}
static void herb(mesh_t m, float u, float v, float z) {
  bf_world_t p = {u, v};
  vessel_band(m, p, z, z + 5, .19f, .24f, 0x9a6e50, 0x685c46);
  for (int i = 0; i < 9; i++) {
    float a = i * tau / 9, du = cosf(a), dv = sinf(a);
    float reach = .33f + (i % 3) * .091f;
    float tip = 13.f + (i % 3) * 1.7f;
    bf_world_t end = {u + du * reach, v + dv * reach};
    bf_world_t mid = {u + du * reach * .61f, v + dv * reach * .61f};
    bf_world_t left = {mid.u - dv * .12f, mid.v + du * .12f};
    bf_world_t right = {mid.u + dv * .12f, mid.v - du * .12f};
    vertex_t l[] = {vertex(p, z + 5.2f), vertex(left, z + tip - .8f),
                    vertex(end, z + tip), vertex(mid, z + tip + .65f)};
    vertex_t r[] = {vertex(p, z + 5.2f), vertex(mid, z + tip + .65f),
                    vertex(end, z + tip), vertex(right, z + tip - 1.3f)};
    face(m, l, 4, pigment(i % 2 ? 0x839a6b : 0x6b9270, 1));
    face(m, r, 4, pigment(i % 2 ? 0x4e7358 : 0x53774e, 1));
  }
  for (int k = 0; k < 2; k++) {
    bf_world_t fruit = {u + (k ? .14f : -.15f), v + (k ? -.08f : .105f)};
    float low = z + (k ? 11.4f : 7.0f);
    vessel_band(m, fruit, low, low + 2.1f, .06f, .11f, 0xc68c40, 0);
    vessel_band(m, fruit, low + 2.1f, low + 4.6f, .11f, .077f, 0xe8ba58,
                0xe6c361);
  }
}
static void lantern(mesh_t m, float u, float v, unsigned body) {
  bf_world_t p = {u, v};
  vessel_band(m, p, 26, 28, .32f, .36f, 0x604d3c, 0xbc9364);
  vessel_band(m, p, 28, 30, .25f, .29f, 0x604b3d, 0x684e3c);
  vessel_band(m, p, 30, 34, .28f, .38f, body, body);
  vessel_band(m, p, 34, 44, .38f, .38f, body, body);
  vessel_band(m, p, 44, 48, .38f, .26f, body, body);
  for (int i = 0; i < 3; i++)
    vessel_band(m, p, 34.5f + i * 3.4f, 35 + i * 3.4f, .385f, .385f, 0xbb775a,
                0);
  vessel_band(m, p, 48, 49.7f, .27f, .23f, 0x66543f, 0xaa865b);
}
static void pendant(mesh_t m, float u, float v, float size) {
  bf_world_t p = {u, v};
  float b = 40, r = .38f * size;
  /* An exposed bracket suspends the shoji light in front of the rear beam. */
  bevel(m, rect(u - .06f, -.05f, .12f, v + .18f, .047f), 63, 65, .5f, 0x594a3c,
        0xb18d64, 0xd7af7b);
  bevel(m, rect(u - .036f, v - .036f, .072f, .072f, .03f), 59, 64, .2f,
        0x463e35, 0x9f845c, 0xa99165);
  vessel_band(m, p, b, b + 2, r * .54f, r * .60f, 0x6c5642, 0xb29365);
  vessel_band(m, p, b + 2, b + 5.1f, r * .59f, r * .94f, 0xd39762, 0);
  vessel_band(m, p, b + 5.1f, b + 8.6f, r * .94f, r * 1.07f, 0xffcc84, 0);
  vessel_band(m, p, b + 8.6f, b + 13.6f, r * 1.07f, r * 1.08f, 0xffe2a1, 0);
  vessel_band(m, p, b + 13.6f, b + 17.2f, r * 1.08f, r * .95f, 0xffdc91, 0);
  vessel_band(m, p, b + 17.2f, b + 19.3f, r * .95f, r * .65f, 0xf2bc7b,
              0xf6d28b);
  for (int i = 0; i < 3; i++) {
    float z = b + 5.2f + i * 4.1f;
    float radial = i == 0 ? r * .96f : r * 1.095f;
    vessel_band(m, p, z, z + .58f, radial, radial, 0xce9b68, 0);
  }
  vessel_band(m, p, b + 19.3f, b + 21, r * .66f, r * .58f, 0x695443, 0xa3855e);
  bevel(m, rect(u - .022f, v - .024f, .044f, .048f, .019f), b - 3, b + .4f, .2f,
        0x945b42, 0xc68452, 0xb5784f);
}

static void bench(mesh_t m, int id) {
  bf_footprint_t r = bf_counters[id];
  shadow(m, r, .35f, 0xbca98e);
  bevel(m, bf_inset(r, .16f), .5f, 5.8f, 1.2f, 0x77593e, 0xbc9269, 0xd1ab7a);
  bevel(m, r, 5.5f, 9.2f, 1.4f, 0xa77b51, 0xd7ac80, 0xf1cfa6);
  for (int i = 0; i < 3; i++) {
    bf_footprint_t seat = rect(r.u + .11f + i * (r.w - .1f) / 3, r.v + .075f,
                               (r.w - .36f) / 3, r.h - .15f, .21f);
    bevel(m, seat, 9.2f, 10.7f, .7f, 0x395953, 0x557b72, 0x89a08b);
    bevel(m, bf_inset(seat, .036f), 10.6f, 12.1f, .8f, 0x597567, 0x6f8e7d,
          0x9cac92);
    if (BF_FRAME_W >= 800) {
      strip(m, (bf_world_t){seat.u + .18f, seat.v + .13f},
            (bf_world_t){seat.u + seat.w - .18f, seat.v + .13f}, .02f, 12.3f,
            0x9cab90);
      strip(m, (bf_world_t){seat.u + .18f, seat.v + seat.h - .13f},
            (bf_world_t){seat.u + seat.w - .18f, seat.v + seat.h - .13f}, .02f,
            12.3f, 0x586f63);
    }
  }
  /* The backrest is part of the bench footprint. */
  bevel(m, rect(r.u + .14f, r.v + r.h - .12f, r.w - .28f, .12f, .055f), 9.2f,
        19.2f, .6f, 0x8e694b, 0xb28c69, 0xd8b389);
  bevel(m, rect(r.u + .19f, r.v + r.h - .15f, r.w - .38f, .15f, .065f), 14.3f,
        20.2f, .65f, 0x596f68, 0x78917e, 0xa1af94);
}
static void crest(mesh_t m, int t, float u, float v) {
  bf_world_t p = {u, v};
  unsigned accent = t ? 0xb25c43 : 0x397a77;
  /* A round inlay sits flush with the island's central tea cabinet. */
  vessel_band(m, p, 22.4f, 23.3f, .70f, .68f, 0x796346, 0xf2d4a3);
  vessel_band(m, p, 23.3f, 23.6f, .61f, .61f, accent, accent);
  if (t) {
    vessel_band(m, p, 23.6f, 24, .18f, .18f, 0xffdda2, 0xffdda2);
    for (int i = 0; i < 8; i++) {
      float a = i * tau / 8;
      strip(m, (bf_world_t){u + .27f * cosf(a), v + .27f * sinf(a)},
            (bf_world_t){u + .44f * cosf(a), v + .44f * sinf(a)}, .06f, 24,
            0xffdda2);
    }
  } else
    for (int j = 0; j < 2; j++) {
      bf_world_t prev = {u - .43f, v + j * .20f - .11f};
      for (int i = 1; i <= 16; i++) {
        float x = i / 16.f;
        bf_world_t next = {u - .43f + .86f * x,
                           v + j * .20f - .11f + .07f * sinf(x * tau)};
        strip(m, prev, next, .060f, 24, 0xe3dbc0);
        prev = next;
      }
    }
}
static void rolling_mat(mesh_t m, float u, float v) {
  /* Parallel bamboo laths and two cotton ties, folded at the working edge. */
  const float z = 22.95f;
  bf_footprint_t r = rect(u, v, 1.15f, .61f, .11f);
  bevel(m, r, z, z + 1.6f, .5f, 0x977250, 0xd4b47d, 0xdec596);
  int laths = W < 800 ? 6 : 12;
  for (int i = 0; i < laths; i++) {
    float x = u + .055f + i * 1.04f / laths;
    strip(m, (bf_world_t){x, v + .06f}, (bf_world_t){x, v + .55f}, .032f,
          z + 1.8f, i % 3 == 0 ? 0xac8d61 : 0xbda375);
  }
  for (int i = 0; i < 2; i++) {
    float y = v + .18f + i * .26f;
    strip(m, (bf_world_t){u + .05f, y}, (bf_world_t){u + 1.09f, y}, .030f,
          z + 2.f, 0xe5dac1);
  }
  for (int i = 0; i < 2; i++) {
    float y = v - .13f - i * .084f;
    strip(m, (bf_world_t){u + .13f, y}, (bf_world_t){u + 1.18f, y - .18f},
          .047f, z + .5f, 0x965843);
  }
}

static void folded_cloth(mesh_t m, float u, float v) {
  /* The stepped hem gives the small silhouette a folded edge at LCD scale. */
  bevel(m, rect(u, v, 1.02f, .57f, .13f), 22.95f, 24.8f, .65f, 0x465d71,
        0x6a858f, 0x9caca3);
  bevel(m, rect(u - .035f, v - .055f, .91f, .52f, .11f), 24.75f, 26.35f, .6f,
        0x547084, 0x698596, 0x9cadad);
  for (int i = 0; i < 2; i++) {
    float y = v + .27f + i * .079f;
    strip(m, (bf_world_t){u + .09f, y}, (bf_world_t){u + .78f, y}, .029f,
          26.55f, 0xc6c5a7);
  }
}

static void island(mesh_t m, int t) {
  bf_footprint_t r = bf_islands[t];
  unsigned accent = t ? 0xb96a50 : 0x4c8077;
  shadow(m, bf_inset(r, .035f), .5f, 0xb4a188);
  bevel(m, bf_inset(r, .14f), .5f, 4.3f, 1.2f, 0x67513f, 0xa77e57, 0xbb9165);
  bevel(m, bf_inset(r, .12f), 3.4f, 5.5f, .7f, 0x5d5245, 0xa98760, 0xc9a781);
  bevel(m, bf_inset(r, .09f), 3.5f, 13.6f, 1.3f, t ? 0x977366 : 0x657a78,
        0xb29170, 0xbba181);
  /* Small vertical ribs follow just the outward apron. */
  int ribs = BF_FRAME_W < 800 ? 48 : 96;
  for (int i = 0; i < ribs; i++) {
    bf_mark_t q = bf_perimeter(bf_inset(r, .071f), i / (float)ribs);
    if (q.normal.v < -.1f) continue;
    bf_world_t a = q.point,
               b = {a.u - q.normal.v * .022f, a.v + q.normal.u * .022f};
    vertex_t stripv[4] = {vertex(a, 4.5f), vertex(b, 4.5f), vertex(b, 12.5f),
                          vertex(a, 12.5f)};
    face(m, stripv, 4, pigment(t ? 0x775b50 : 0x475d5d, 1));
  }
  bevel(m, r, 13, 17.7f, 1.7f, 0x80573e, 0xf3d4a4, 0xffe3b7);
  roof(m, bf_inset(r, .105f), 17.78f, 0x6e665b);
  roof(m, bf_inset(r, .145f), 17.90f, 0xd9dbc9);
  roof(m, bf_inset(r, .17f), 18, 0x8fa4a7);
  /* The polished outer shoulder shares the path with the slats. Its fixed
   * sky and window reflections reveal curvature at both ends of the loop. */
  bf_footprint_t shoulder = bf_inset(r, .145f);
  int sections = BF_FRAME_W < 800 ? 48 : 112;
  for (int i = 0; i < sections; i++) {
    bf_mark_t a = bf_perimeter(shoulder, i / (float)sections);
    bf_mark_t b = bf_perimeter(shoulder, (i + 1) / (float)sections);
    float reflection = .5f + .5f * (-a.normal.u * .72f - a.normal.v * .69f);
    float light = .72f + .44f * reflection * reflection;
    unsigned metal = 0xbcd0cf;
    bf_world_t p = {a.point.u + .046f * a.normal.u,
                    a.point.v + .046f * a.normal.v};
    bf_world_t q = {b.point.u + .046f * b.normal.u,
                    b.point.v + .046f * b.normal.v};
    vertex_t facets[] = {vertex(p, 17.93f), vertex(q, 17.93f),
                         vertex(b.point, 18.07f), vertex(a.point, 18.07f)};
    face(m, facets, 4, pigment(metal, light));
  }
  roof(m, bf_inset(r, .83f), 18.1f, 0xd7d4be);
  roof(m, bf_inset(r, .88f), 18.4f, 0x876c4b);
  bf_footprint_t core = bf_inset(r, .95f);
  core.r = .54f;
  bevel(m, core, 18, 22.4f, 1.45f, 0xb58d63, 0xe7c799, 0xf9dcab);
  /* Two islands share joinery and proportions but have their own cloth,
   * service emblem and tea equipment. All are inside the lane. */
  bf_footprint_t runner = bf_inset(core, .24f);
  runner.r = .35f;
  roof(m, runner, 22.55f, t ? 0x975f4e : 0x537878);
  roof(m, bf_inset(runner, .065f), 22.8f, t ? 0xc99779 : 0x84a597);
  /* Two short stitching runs stay below the condiment tray and house stamp. */
  if (BF_FRAME_W >= 800)
    for (int i = 0; i < 12; i++) {
      float u = runner.u + .22f + i * (runner.w - .41f) / 12.f;
      for (int edge = 0; edge < 2; edge++) {
        float v = runner.v + (edge ? runner.h - .14f : .14f);
        strip(m, (bf_world_t){u, v}, (bf_world_t){u + .095f, v}, .018f, 22.97f,
              0xc8ba96);
      }
    }
  bf_footprint_t tray = rect(r.u + 1.35f, r.v + 1.44f, 3.27f, 1.01f, .20f);
  bevel(m, tray, 22.81f, 23.4f, .38f, 0x6b5442, 0x9b7359, 0xc09a70);
  roof(m, bf_inset(tray, .07f), 23.53f, 0x7e6656);
  crest(m, t, r.u + 3, r.v + 3.24f);
  bf_world_t teapot = {r.u + 4.05f, r.v + 2.00f};
  vessel_band(m, teapot, 22.9f, 25, .17f, .29f, accent, accent);
  vessel_band(m, teapot, 25, 29, .29f, .27f, accent, 0xc5bf9e);
  vessel_band(m, teapot, 29, 30, .24f, .12f, accent, 0x839580);
  vessel_band(m, teapot, 30, 31.8f, .077f, .077f, 0x635b49, 0xb3a47f);
  bevel(m, rect(teapot.u - .39f, teapot.v - .08f, .20f, .16f, .07f), 25, 27.8f,
        1, accent, 0xa8b59e, accent);
  cups(m, r.u + 1.75f, r.v + 2, 23.7f);
  bf_world_t jar = {r.u + 2.94f, r.v + 1.98f};
  vessel_band(m, jar, 23.7f, 25.0f, .16f, .22f, 0xa3a494, 0);
  vessel_band(m, jar, 25.0f, 29.1f, .24f, .23f, 0xe4dabf, 0xe3d9bd);
  vessel_band(m, jar, 29.1f, 30.1f, .25f, .22f, 0xab7455, 0xc08b68);
  vessel_band(m, jar, 30.1f, 32.0f, .07f, .064f, 0x976c53, 0xceaa7c);
  if (t == 0)
    rolling_mat(m, r.u + 1.72f, r.v + 4.05f);
  else
    folded_cloth(m, r.u + 3.55f, r.v + 4.09f);
}
static void sauce_pair(mesh_t m, float u, float v, float z) {
  /* One cruet silhouette and a squat ginger pot. Their low tray is behind
   * the food row. Local colour lets the outline read on plain wood. */
  bevel(m, rect(u - .42f, v - .27f, 1.14f, .58f, .16f), z, z + 1, .5f, 0x694f42,
        0x856452, 0xb18b63);
  bf_world_t soy = {u - .08f, v};
  vessel_band(m, soy, z + 1, z + 3, .105f, .15f, 0x764f3b, 0);
  vessel_band(m, soy, z + 3, z + 8.8f, .15f, .13f, 0x9a623c, 0);
  vessel_band(m, soy, z + 8.8f, z + 11.2f, .13f, .069f, 0xc3966a, 0);
  vessel_band(m, soy, z + 11.2f, z + 13.8f, .085f, .099f, 0xb9573f, 0xe6996e);
  bevel(m, rect(u - .015f, v - .06f, .19f, .06f, .025f), z + 10.6f, z + 11.8f,
        .25f, 0xa57852, 0xd7b38a, 0xdcc5a4);
  bf_world_t pot = {u + .40f, v + .035f};
  vessel_band(m, pot, z + 1, z + 2.1f, .12f, .16f, 0xb59c78, 0);
  vessel_band(m, pot, z + 2.1f, z + 5.9f, .17f, .155f, 0xd2c5a4, 0xc6b58f);
  vessel_band(m, pot, z + 5.9f, z + 6.7f, .175f, .155f, 0xa76a50, 0xce9d70);
  vessel_band(m, pot, z + 6.7f, z + 8.0f, .050f, .044f, 0x986349, 0xb48763);
}

static void light_lanterns(mesh_t m) {
  /* The paper itself is emissive. Only the surrounding architecture receives
   * the window shadow. These solids are still part of the static cache. */
  lantern(m, 1.0f, .79f, 0xffc07b);
  lantern(m, 18.82f, .79f, 0xffca84);
  pendant(m, 4.55f, .95f, .96f);
  pendant(m, 15.12f, .95f, 1.05f);
}

void bf_build_room(bf_canvas_t c) {
  memset(c.heights, 0, W * H);
  mesh_t m = {c, true};
  floor_and_steps(m);
  back_wall(m);
  noren(m, 4.29f, 0xa95147);
  noren(m, 13.03f, 0x386878);
  pass_shelves(m);
  rice_tub(m, 3.90f, .64f, 26.55f);
  for (int i = 0; i < 3; i++) cabinet(m, i);
  herb(m, 5.92f, .84f, 26.5f);
  herb(m, 14.21f, .84f, 26.5f);
  sauce_pair(m, 6.85f, .88f, 26.5f);
  sauce_pair(m, 13.24f, .88f, 26.5f);
  for (int t = 0; t < BF_LOOPS; t++) island(m, t);
  bench(m, 3);
  bench(m, 4);
  bf_light_room(c);
  light_lanterns(m);
}
void bf_move_tracks(bf_canvas_t c, const bf_game_t* g) {
  mesh_t m = {c, false};
  for (int t = 0; t < BF_LOOPS; t++) {
    bf_footprint_t r = bf_inset(bf_islands[t], .47f);
    float phase = bf_loop_phase(t, g->elapsed_ms);
    /* A slat seam crosses one lane. The two adjoining corners use the same
     * tangent and arc-length parameter as every plate. */
    for (int i = 0; i < 64; i++) {
      bf_mark_t q = bf_perimeter(r, phase + i / 64.f);
      bf_world_t p = q.point, n = q.normal;
      bf_world_t a = {p.u - .319f * n.u, p.v - .319f * n.v},
                 b = {p.u + .315f * n.u, p.v + .315f * n.v};
      strip(m, a, b, .018f, 18.2f, 0x7e8e85);
      if (i % 8 == 0) {
        bf_world_t d = {p.u + .24f * n.u, p.v + .24f * n.v};
        strip(m, d, b, .085f, 18.25f, t ? 0xb57555 : 0x527e73);
      }
    }
  }
}
