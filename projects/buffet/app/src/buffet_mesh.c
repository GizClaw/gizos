/* Orthographic room solids. The height of every visible sample is cached with
 * the static room and also clips the walker's billboard against counter lips.
 */
#include "buffet_mesh.h"
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
    float shade = .90f + (-.17f * dv + .13f * -du) / len;
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
  enum { N = 32 };
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
  for (int i = 0; i < count; i++) {
    vessel_band(m, p, z, z + 1.5f, .29f, .42f, 0xb1bca8, 0xf4ead1);
    z += 1.6f;
  }
  vessel_band(m, p, z, z + .35f, .29f, .29f, 0xb3c7bb, 0xc6d5c1);
}
static void cups(mesh_t m, float u, float v, float z) {
  for (int i = 0; i < 2; i++) {
    bf_world_t p = {u + i * .40f, v};
    vessel_band(m, p, z, z + 5, .13f, .16f, 0x57786e, 0xe3d7b7);
    vessel_band(m, p, z + 5, z + 5.3f, .115f, .115f, 0x71543a, 0x71543a);
  }
}
static void shadow(mesh_t m, bf_footprint_t r, float z, unsigned color) {
  r.u += .075f;
  r.v += .10f;
  roof(m, bf_inset(r, -.08f), z + .18f, color);
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
  static const unsigned tiles[] = {0xe9dbc5, 0xe6d8bf, 0xebdec8, 0xeadcc5,
                                   0xe5d6bb};
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
  bevel(m, rect(-.02f, -.20f, 20.02f, .45f, .21f), 2, 45, 1.7f, 0xbfa783,
        0x79593d, 0xefce99);
  bevel(m, rect(-.06f, -.25f, 20.12f, .49f, .22f), 44, 48, 1.3f, 0x573e32,
        0x906746, 0xe1aa71);
  /* Translucent shoji panels and their individual timber mullions. */
  for (int j = 0; j < 4; j++) {
    float u = .54f + j * 4.96f;
    bevel(m, rect(u, .235f, 4.03f, .10f, .04f), 27, 42, 1, 0xb7d1bd, 0xeee6c1,
          0xddc9a1);
    bevel(m, rect(u, .34f, 4.02f, .065f, .027f), 32, 33.2f, .4f, 0xb39b72,
          0xdebf89, 0xe9ce9a);
    for (int k = 0; k < 6; k++)
      bevel(m, rect(u + .035f + k * .78f, .335f, .046f, .070f, .02f), 27, 42,
            .35f, 0xa58e6d, 0xe2c998, 0xe1c39c);
    bevel(m, rect(u - .06f, .28f, 4.18f, .18f, .08f), 25.5f, 27, .6f, 0x8c6847,
          0xdcb681, 0xeac28e);
  }
  for (int side = 0; side < 2; side++)
    bevel(m, rect(side ? 19.78f : -.10f, .18f, .33f, 10.45f, .15f), 0, 15, 1.2f,
          0x9f7252, 0xc69666, 0xf2cd99);
}
static void cabinet(mesh_t m, int row) {
  bf_footprint_t r = bf_counters[row];
  bool along = row == 0;
  float z = along ? 8 : 0;
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
      roof(m, rect(r.u + .13f, start + .13f, r.w - .26f, pitch - .26f, .17f),
           18.38f, row == 1 ? 0xcd9a7d : 0xa4acb9);
    }
  }
  if (!along) return;
  for (int i = 0; i < 3; i++)
    roof(m, rect(8.5f + 1.5f * i - .54f, .38f, 1.08f, .91f, .11f), 26.4f,
         0x86a58d);
  dish_stack(m, 2.48f, .84f, 26.4f, 5);
  dish_stack(m, 3.21f, .84f, 26.4f, 3);
  cups(m, 5.10f, .85f, 26.4f);
  dish_stack(m, 15.23f, .84f, 26.4f, 4);
  cups(m, 16.42f, .83f, 26.4f);
}
static void noren(mesh_t m, float u, unsigned cloth) {
  const float v = .49f;
  const unsigned hem = 0xf1d6a0;
  bevel(m, rect(u - .1f, v - .11f, 3.96f, .14f, .06f), 46, 48, .7f, 0x6a503c,
        0xcda878, 0xeac594);
  for (int j = 0; j < 4; j++) {
    float x = u + j * .96f, z = 33.6f + ((j == 0 || j == 3) ? .9f : 0);
    bevel(m, rect(x, v, .91f, .10f, .045f), z, 46.2f, .45f, cloth, cloth,
          cloth);
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
  for (int i = 0; i < 7; i++) {
    float a = i * tau / 7, du = cosf(a), dv = sinf(a),
          tip = 8.5f + (i % 3) * 1.3f;
    bf_world_t tippos = {u + du * .38f, v + dv * .38f},
               left = {u + du * .18f - dv * .095f, v + dv * .18f + du * .095f},
               right = {u + du * .18f + dv * .095f, v + dv * .18f - du * .095f};
    vertex_t leaf[4] = {vertex(p, z + 4.5f), vertex(left, z + 8),
                        vertex(tippos, z + tip), vertex(right, z + 8)};
    face(m, leaf, 4, pigment(i % 2 ? 0x46735a : 0x749467, 1));
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
static void bench(mesh_t m, int id) {
  bf_footprint_t r = bf_counters[id];
  shadow(m, r, .35f, 0xbca98e);
  bevel(m, bf_inset(r, .16f), .5f, 5.8f, 1.2f, 0x77593e, 0xbc9269, 0xd1ab7a);
  bevel(m, r, 5.5f, 9.2f, 1.4f, 0xa77b51, 0xd7ac80, 0xf1cfa6);
  for (int i = 0; i < 3; i++) {
    bf_footprint_t seat = rect(r.u + .11f + i * (r.w - .1f) / 3, r.v + .075f,
                               (r.w - .36f) / 3, r.h - .15f, .21f);
    bevel(m, seat, 9.2f, 11.2f, 1, 0x587169, 0x739183, 0x9bb29d);
  }
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
static void island(mesh_t m, int t) {
  bf_footprint_t r = bf_islands[t];
  unsigned accent = t ? 0xb96a50 : 0x4c8077;
  shadow(m, bf_inset(r, .035f), .5f, 0xb4a188);
  bevel(m, bf_inset(r, .14f), .5f, 4.3f, 1.2f, 0x67513f, 0xa77e57, 0xbb9165);
  bevel(m, bf_inset(r, .08f), 3.5f, 13.6f, 1.6f, 0xad8059, 0xc59b72, 0xd7b184);
  /* Small vertical ribs follow just the outward apron. */
  for (int i = 0; i < 120; i++) {
    bf_mark_t q = bf_perimeter(bf_inset(r, .071f), i / 120.f);
    if (q.normal.v < -.1f) continue;
    bf_world_t a = q.point,
               b = {a.u - q.normal.v * .022f, a.v + q.normal.u * .022f};
    vertex_t stripv[4] = {vertex(a, 4.5f), vertex(b, 4.5f), vertex(b, 12.5f),
                          vertex(a, 12.5f)};
    face(m, stripv, 4, pigment(0x916946, 1));
  }
  bevel(m, r, 13, 17.7f, 1.7f, 0x80573e, 0xf3d4a4, 0xffe3b7);
  roof(m, bf_inset(r, .115f), 17.8f, 0x706c5c);
  roof(m, bf_inset(r, .17f), 18, 0xa4aaa1);
  roof(m, bf_inset(r, .83f), 18.1f, 0xd7d4be);
  roof(m, bf_inset(r, .88f), 18.4f, 0x876c4b);
  bf_footprint_t core = bf_inset(r, .95f);
  core.r = .54f;
  bevel(m, core, 18, 22.4f, 1.45f, 0xb58d63, 0xe7c799, 0xf9dcab);
  /* Two islands share joinery and proportions but have their own cloth,
   * service emblem and tea equipment. All are inside the lane. */
  bf_footprint_t runner = bf_inset(core, .24f);
  runner.r = .35f;
  roof(m, runner, 22.55f, t ? 0xce9475 : 0x91ad98);
  roof(m, bf_inset(runner, .07f), 22.8f, t ? 0xd9a484 : 0x9fb9a1);
  crest(m, t, r.u + 3, r.v + 3.24f);
  bf_world_t teapot = {r.u + 4.05f, r.v + 2.00f};
  vessel_band(m, teapot, 22.9f, 25, .17f, .29f, accent, accent);
  vessel_band(m, teapot, 25, 29, .29f, .27f, accent, 0xc5bf9e);
  vessel_band(m, teapot, 29, 30, .24f, .12f, accent, 0x839580);
  vessel_band(m, teapot, 30, 31.8f, .077f, .077f, 0x635b49, 0xb3a47f);
  bevel(m, rect(teapot.u - .39f, teapot.v - .08f, .20f, .16f, .07f), 25, 27.8f,
        1, accent, 0xa8b59e, accent);
  cups(m, r.u + 1.75f, r.v + 2, 22.85f);
}
void bf_build_room(bf_canvas_t c) {
  memset(c.heights, 0, W * H);
  mesh_t m = {c, true};
  floor_and_steps(m);
  back_wall(m);
  noren(m, 5.45f, 0xa95c46);
  noren(m, 12.90f, 0x416a64);
  for (int i = 0; i < 3; i++) cabinet(m, i);
  herb(m, 5.92f, .84f, 26.5f);
  herb(m, 14.21f, .84f, 26.5f);
  lantern(m, 1.0f, .79f, 0xe89a6f);
  lantern(m, 18.82f, .79f, 0xe5a172);
  for (int t = 0; t < BF_LOOPS; t++) island(m, t);
  bench(m, 3);
  bench(m, 4);
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
