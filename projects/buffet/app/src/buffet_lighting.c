#include "buffet_lighting.h"

#include "buffet_rotary.h"
#include "buffet_scale.h"
#include <math.h>
#include <stdint.h>

enum { LIGHT_LEVELS = BF_FRAME_W < 800 ? 16 : 32 };

/* The paper windows and the two lanterns share the light used by the painted
 * food. Sample illumination on a coarse lattice once when the room is cached.
 */
static float smooth(float value) {
  float x = fminf(1, fmaxf(0, value));
  return x * x * (3.f - 2.f * x);
}

static float window_bar(float x, float left, float right, float feather) {
  return smooth((x - left) / feather) * smooth((right - x) / feather);
}

static float lamp_pool(float u, float v, float center_u, float center_v) {
  float du = (u - center_u) / 2.3f, dv = (v - center_v) / 2.1f;
  float d = fmaxf(0, 1 - du * du - dv * dv);
  return d * d;
}

static int illumination(int pixel_x, int pixel_y) {
  bf_point_t screen = {(float)pixel_x * BF_SCALE_DEN / BF_SCALE_NUM,
                       (float)pixel_y * BF_SCALE_DEN / BF_SCALE_NUM};
  bf_world_t p = bf_unproject(screen);
  float u = p.u, v = p.v;
  /* Long sun shafts cross the floor at the angle of the shoji. A couple of
   * mullions and the high sill break up the field, even at LCD resolution. */
  float projection = u - .53f * v;
  float pane = window_bar(projection, -.2f, 4.65f, .18f) +
               .66f * window_bar(projection, 5.06f, 8.30f, .22f);
  float mullions = 1 - .53f * window_bar(projection, 1.14f, 1.61f, .15f) -
                   .46f * window_bar(projection, 2.74f, 3.25f, .16f);
  float sill = 1 - .54f * window_bar(v + .10f * u, 5.11f, 5.72f, .19f);
  float sunlight = pane * mullions * sill * window_bar(v, -.8f, 10.5f, 1.5f);
  float lamps = lamp_pool(u, v, 1.05f, -.55f) + lamp_pool(u, v, 4.55f, -.88f) +
                lamp_pool(u, v, 15.12f, -.88f) + lamp_pool(u, v, 18.82f, -.64f);
  float openness = .26f * smooth((v + 1.5f) / 8.f) - .16f * smooth(u / 20.f);
  float value = .33f + openness + .58f * sunlight + .44f * lamps;
  return (int)lroundf((LIGHT_LEVELS - 1) * fminf(1, fmaxf(0, value)) * 256.f);
}

/* Use native RGB565 channels to tint light and reflected shade. */
static unsigned channel(float gain, float lift, int value, int limit) {
  return (unsigned)fminf(limit, fmaxf(0, value * gain + lift + .5f));
}

void bf_light_room(bf_canvas_t canvas) {
  enum {
    step = BF_FRAME_W >= 800 ? 10 : 8,
    columns = (BF_FRAME_W + step - 1) / step + 1
  };
  uint8_t red[LIGHT_LEVELS][32], green[LIGHT_LEVELS][64],
      blue[LIGHT_LEVELS][32];
  for (int i = 0; i < LIGHT_LEVELS; i++) {
    float t = (float)i / (LIGHT_LEVELS - 1);
    for (int c = 0; c < 32; c++) {
      red[i][c] = (uint8_t)channel(.56f + .65f * t, .2f + .55f * t, c, 31);
      blue[i][c] = (uint8_t)channel(.82f + .17f * t, 1.40f - 1.60f * t, c, 31);
    }
    for (int c = 0; c < 64; c++)
      green[i][c] = (uint8_t)channel(.67f + .44f * t, .8f + .38f * t, c, 63);
  }
  static const uint8_t dither[4][4] = {{0, 128, 32, 160},
                                       {192, 64, 224, 96},
                                       {48, 176, 16, 144},
                                       {240, 112, 208, 80}};
  int upper[columns], lower[columns];
  for (int k = 0; k < columns; k++) upper[k] = illumination(k * step, 0);
  for (int by = 0; by < BF_FRAME_H; by += step) {
    for (int k = 0; k < columns; k++)
      lower[k] = illumination(k * step, by + step);
    for (int y = by; y < by + step && y < BF_FRAME_H; y++) {
      int v = y - by;
      for (int k = 0; k < columns - 1; k++) {
        int start = upper[k] + (lower[k] - upper[k]) * v / step;
        int end = upper[k + 1] + (lower[k + 1] - upper[k + 1]) * v / step;
        int dx = (end - start) / step;
        int x0 = k * step, x1 = x0 + step;
        if (x1 > BF_FRAME_W) x1 = BF_FRAME_W;
        for (int x = x0, value = start; x < x1; x++, value += dx) {
          int pixel = y * BF_FRAME_W + x;
          if (!canvas.heights[pixel]) continue;
          int level = (value + dither[y & 3][x & 3]) >> 8;
          uint16_t color = canvas.pixels[pixel];
          canvas.pixels[pixel] =
              (uint16_t)((red[level][color >> 11] << 11) |
                         (green[level][(color >> 5) & 63] << 5) |
                         blue[level][color & 31]);
        }
      }
    }
    for (int k = 0; k < columns; k++) upper[k] = lower[k];
  }
}
