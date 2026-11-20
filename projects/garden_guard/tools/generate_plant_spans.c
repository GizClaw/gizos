#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "capture.c"

/* Blob index. All variants begin from the actual cell projection. */
enum {
  WIDTH = 480,
  HEIGHT = 320,
  CELLS = LANES * COLS,
  SLOTS = 34 * 2,
  MAX_OVERLAP = 32
};
typedef struct {
  uint16_t color, alpha;
} ink_t;
typedef struct {
  unsigned n;
  ink_t inks[MAX_OVERLAP];
} pixel_t;
static pixel_t pixels[HEIGHT][WIDTH];
static uint16_t framebuffer[HEIGHT][WIDTH];
static unsigned max_overlap;
static uint32_t offsets[CELLS][SLOTS], frame_sizes[CELLS][SLOTS];
static uint32_t pose_bits[CELLS][SLOTS], projection[CELLS][3];
static uint32_t frame_offset;
static unsigned frames;

static uint32_t bits(float f) {
  uint32_t b;
  memcpy(&b, &f, sizeof(b));
  return b;
}

static void capture_pixel(int32_t x, int32_t y, uint16_t color,
                          uint32_t alpha) {
  if (alpha == 0u) return;
  assert(x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT && alpha <= 256u);
  pixel_t* p = &pixels[y][x];
  /* One depth test protects the entire sequence. Opaque pixels make earlier
     brush strokes irrelevant, and every later blend uses RGB565 rounding. */
  if (alpha == 256u) {
    p->n = 1u;
    p->inks[0] = (ink_t){color, 256u};
    return;
  }
  if (p->n != 0u && p->inks[0].alpha == 256u) {
    p->inks[0].color = blend(p->inks[0].color, color, alpha);
    return;
  }
  assert(p->n < MAX_OVERLAP);
  p->inks[p->n++] = (ink_t){color, (uint16_t)alpha};
  if (p->n > max_overlap) max_overlap = p->n;
}

static void put8(FILE* f, unsigned v) {
  assert(v < 256u && fputc((int)v, f) != EOF);
}
static void put16(FILE* f, unsigned v) {
  put8(f, v & 255u);
  put8(f, v >> 8);
}

typedef struct {
  uint16_t color, alpha;
} palette_t;

static unsigned palette_index(palette_t* palette, unsigned* count, ink_t ink) {
  for (unsigned i = 0; i < *count; ++i) {
    if (palette[i].color == ink.color && palette[i].alpha == ink.alpha)
      return i;
  }
  assert(*count < 32768u);
  palette[*count] = (palette_t){ink.color, ink.alpha};
  return (*count)++;
}

static void encode(FILE* f, const h2_garden_guard_canvas_t* canvas, float gx,
                   float gy) {
  const long frame_start = ftell(f);
  const int32_t ox = (int32_t)gx, oy = (int32_t)gy;
  unsigned band_count = 0u;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_BANDS; ++i)
    if ((canvas->bands >> i) & 1u) ++band_count;
  put8(f, band_count);
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_BANDS; ++i) {
    if (((canvas->bands >> i) & 1u) == 0u) continue;
    put8(f, i);
    put8(f, (unsigned)(canvas->band_x0[i] - ox + 128));
    put8(f, (unsigned)(canvas->band_x1[i] - ox + 128));
  }

  const int32_t x0 = ox - 40 < 0 ? 0 : ox - 40;
  const int32_t x1 = ox + 40 > WIDTH ? WIDTH : ox + 40;
  const int32_t y0 = oy - 64 < 0 ? 0 : oy - 64;
  const int32_t y1 = oy + 12 > HEIGHT ? HEIGHT : oy + 12;
  unsigned colors = 0u, edge_colors = 0u;
  palette_t palette[1024], edges[128];
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      assert(y >= 0 && y < HEIGHT && x >= 0 && x < WIDTH);
      const pixel_t* p = &pixels[y][x];
      if (p->n == 1u && p->inks[0].alpha == 256u)
        (void)palette_index(palette, &colors, p->inks[0]);
      else
        for (unsigned i = 0; i < p->n; ++i) {
          assert(edge_colors < 128u);
          (void)palette_index(edges, &edge_colors,
                              (ink_t){p->inks[i].color, 0u});
        }
    }
  }
  const bool indexed = colors <= 255u;
  put8(f, indexed ? colors : 0u);
  if (indexed)
    for (unsigned i = 0; i < colors; ++i) put16(f, palette[i].color);
  put8(f, edge_colors);
  for (unsigned i = 0; i < edge_colors; ++i) put16(f, edges[i].color);
  size_t consumed = 0u;
  for (int32_t y = y0; y < y1; ++y) {
    bool emitted_row = false;
    for (int32_t x = x0; x < x1;) {
      if (pixels[y][x].n == 0u) {
        ++x;
        continue;
      }
      if (!emitted_row) {
        put8(f, (unsigned)(y - oy + 128));
        emitted_row = true;
      }
      const pixel_t* p = &pixels[y][x];
      const bool solid = p->n == 1u && p->inks[0].alpha == 256u;
      int32_t end = x + 1;
      while (end < x1) {
        const pixel_t* next = &pixels[y][end];
        if (next->n == 0u ||
            (next->n == 1u && next->inks[0].alpha == 256u) != solid)
          break;
        ++end;
      }
      // Retain the original packet boundaries, with a per-packet encoding flag.
      put8(f, (unsigned)(x - ox + 128));
      size_t repeats = 0;
      if (solid)
        for (int32_t i = x + 1; i < end; ++i)
          repeats +=
              pixels[y][i - 1].inks[0].color == pixels[y][i].inks[0].color;
      const bool rle = solid && repeats * 2u >= (size_t)(end - x);
      assert(end - x < 128);
      put8(f, (unsigned)(end - x) | (solid ? 128u : 0u));
      if (solid) put8(f, (unsigned)rle);
      if (rle) {
        while (x < end) {
          const ink_t value = pixels[y][x].inks[0];
          int32_t next = x + 1;
          while (next < end && pixels[y][next].inks[0].color == value.color)
            ++next;
          put8(f, (unsigned)(next - x));
          if (indexed) {
            const unsigned old_colors = colors;
            put8(f, palette_index(palette, &colors, value));
            assert(colors == old_colors);
          } else
            put16(f, value.color);
          consumed += (size_t)(next - x);
          x = next;
        }
      }
      for (; x < end; ++x) {
        const pixel_t* px = &pixels[y][x];
        ++consumed;
        if (solid && !indexed) {
          put16(f, px->inks[0].color);
        } else if (solid) {
          const unsigned old_colors = colors;
          put8(f, palette_index(palette, &colors, px->inks[0]));
          assert(colors == old_colors);
        } else {
          for (unsigned i = 0; i < px->n; ++i) {
            put8(f, px->inks[i].alpha);
            const unsigned old_colors = edge_colors;
            const unsigned key = palette_index(edges, &edge_colors,
                                               (ink_t){px->inks[i].color, 0u});
            assert(edge_colors == old_colors);
            put8(f, key | (i + 1u == px->n ? 128u : 0u));
          }
        }
      }
    }
    if (emitted_row) put8(f, 0u);
  }
  put8(f, 0u);
  for (int32_t y = 0; y < HEIGHT; ++y)
    for (int32_t x = 0; x < WIDTH; ++x)
      if (pixels[y][x].n) {
        assert(consumed);
        --consumed;
      }
  assert(consumed == 0u);
  long frame_end = ftell(f);
  assert(frame_start >= 0 && frame_end > frame_start && frame_end < UINT32_MAX);
  frame_offset = (uint32_t)frame_end;
  ++frames;
}

static void write_u32_table(FILE* f, const char* name, const uint32_t* v,
                            unsigned rows, unsigned cols) {
  fprintf(f, "static const uint32_t %s[%u][%u] = {\n", name, rows, cols);
  for (unsigned row = 0; row < rows; ++row) {
    fprintf(f, " {");
    for (unsigned i = 0; i < cols; ++i) fprintf(f, " %uu,", v[row * cols + i]);
    fprintf(f, " },\n");
  }
  fprintf(f, "};\n");
}

int main(int argc, char** argv) {
  assert(argc == 3 || argc == 4);
  FILE* f = fopen(argv[1], "wb");
  assert(f);
  FILE* header = fopen(argv[2], "w");
  assert(header);
  FILE* metrics = argc == 4 ? fopen(argv[3], "w") : NULL;
  h2_garden_guard_game_t game;
  h2_garden_guard_game_init(&game, WIDTH, HEIGHT, 194021u, 0u);
  for (unsigned lane = 0; lane < LANES; ++lane)
    for (unsigned col = 0; col < COLS; ++col) {
      const unsigned cell = lane * COLS + col;
      float x, y;
      h2_garden_guard_field_point(&game.layout, lane, (float)col + 0.5f, &x,
                                  &y);
      const float s = h2_garden_guard_lane_size(&game.layout, lane);
      projection[cell][0] = bits(x);
      projection[cell][1] = bits(y);
      projection[cell][2] = bits(s);
      for (unsigned kind = 1u; kind <= 2u; ++kind) {
        const unsigned period = kind == 1u ? 20u : 14u;
        const unsigned repeat = kind == 1u ? 140u : 154u;
        h2_garden_guard_plant_t* plant = &game.plants[lane][col];
        *plant = (h2_garden_guard_plant_t){.kind = (uint8_t)kind};
        for (unsigned tick = 0; tick < repeat; ++tick) {
          game.time_ms = tick * H2_GARDEN_GUARD_PLANT_IDLE_MS;
          const h2_garden_guard_plant_pose_t pose =
              h2_garden_guard_plant_pose(&game, lane, col);
          const unsigned slot = 2u * ((kind == 1u ? 0u : 20u) + tick % period) +
                                (unsigned)pose.blink;
          if (offsets[cell][slot] != 0u) {
            assert(pose_bits[cell][slot] == bits(pose.sway));
            continue;
          }
          assert(pose.action == 0.0f && pose.growth == 1.0f &&
                 pose.lift == 0.0f);
          h2_garden_guard_canvas_t canvas = {.width = WIDTH,
                                             .height = HEIGHT,
                                             .stride_pixels = WIDTH,
                                             .pixels = &framebuffer[0][0]};
          field_clip(&canvas, &game.layout);
          memset(pixels, 0, sizeof(pixels));
          draw_plant_kind(&canvas, plant->kind, x, y, s, 0u, pose, false);
          pose_bits[cell][slot] = bits(pose.sway);
          const uint32_t start = frame_offset;
          offsets[cell][slot] = start + 1u;
          encode(f, &canvas, x, y);
          frame_sizes[cell][slot] = frame_offset - start;
          if (metrics)
            fprintf(metrics, "%u %u %u %u %u\n", lane, col, kind, slot,
                    frame_sizes[cell][slot]);
        }
      }
    }
  assert(fclose(f) == 0);
  h2_garden_guard_canvas_t expected = {.width = WIDTH, .height = HEIGHT};
  field_clip(&expected, &game.layout);
  fprintf(
      header,
      "enum { BANK_W = %d, BANK_H = %d, BANK_BYTES = %uu, BANK_CLIP_Y0 = %d, BANK_CLIP_Y1 = %d };\n",
      WIDTH, HEIGHT, frame_offset, expected.clip_y0, expected.clip_y1);
  write_u32_table(header, "bank_offsets", &offsets[0][0], CELLS, SLOTS);
  write_u32_table(header, "bank_sway", &pose_bits[0][0], CELLS, SLOTS);
  write_u32_table(header, "bank_projection", &projection[0][0], CELLS, 3u);
  assert(fclose(header) == 0);
  if (metrics) assert(fclose(metrics) == 0);
  printf("frames=%u bank_bytes=%u index_bytes=%zu maximum_partial_overlap=%u\n",
         frames, frame_offset,
         sizeof(offsets) + sizeof(pose_bits) + sizeof(projection), max_overlap);
}
