#include "h2_g99_brand.h"

#include <math.h>
#include <stddef.h>

/* Rounds half up, as the design reference does. */
static int32_t round_i(float value) { return (int32_t)floorf(value + 0.5f); }

static int32_t max_i(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t min_i(int32_t a, int32_t b) { return a < b ? a : b; }

bool h2_g99_brand_layout(int32_t width, int32_t height,
                         h2_g99_brand_layout_t* out) {
  if (out == NULL || width <= 0 || height <= 0) return false;
  const int32_t short_side = min_i(width, height);
  const float k = (float)short_side / 240.0f;
  h2_g99_brand_layout_t l = {0};
  l.width = width;
  l.height = height;

  /* Status line above a bottom-flush bar. */
  const int32_t pad = round_i(12.0f * k);
  l.status_x = pad;
  l.bar_h = max_i(2, round_i(10.0f * k));
  l.bar_y = height - l.bar_h;
  l.status_font = max_i(12, round_i(11.0f * k));
  l.status_baseline =
      l.bar_y - round_i(8.0f * k) - round_i((float)l.status_font * 0.22f);
  l.status_top = l.status_baseline - round_i((float)l.status_font * 0.78f);

  /* Brand group: icon, gap, wordmark and its shadow. */
  l.cell = max_i(1, short_side / 96);
  l.shadow = max_i(1, l.cell / 2);
  l.icon_h = 12 * l.cell;
  l.icon_w = round_i(92.0f / 120.0f * (float)l.icon_h);
  l.icon_image_w = round_i((float)l.icon_h * 96.0f / 120.0f);
  l.icon_image_h = round_i((float)l.icon_h * 124.0f / 120.0f);
  l.gap = 3 * l.cell;
  l.wordmark_image_w = H2_G99_WORDMARK_COLS * l.cell + l.shadow;
  l.wordmark_image_h = H2_G99_WORDMARK_ROWS * l.cell + l.shadow;
  const int32_t group_h = l.icon_h + l.gap + l.wordmark_image_h;
  /* Centred, but never closer to the status line than one padding. */
  l.group_y = min_i(round_i((float)(height - group_h) / 2.0f),
                    l.status_top - pad - group_h);
  l.icon_x = round_i((float)(width - l.icon_w) / 2.0f);
  l.wordmark_x = round_i((float)(width - H2_G99_WORDMARK_COLS * l.cell) / 2.0f);
  l.wordmark_y = l.group_y + l.icon_h + l.gap;
  if (l.group_y < 0 || l.wordmark_x < 0 || l.icon_x < 0) return false;
  *out = l;
  return true;
}
