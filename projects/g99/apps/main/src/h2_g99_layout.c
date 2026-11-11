#include "h2_g99_layout.h"

#include <math.h>
#include <stddef.h>

static const float kIconSizes[3] = {34.0f, 22.0f, 15.0f};
static const float kThumbSizes[3] = {50.0f, 30.0f, 20.0f};
static const float kOpacities[3] = {1.0f, 0.55f, 0.22f};

static int32_t round_i(float value) { return (int32_t)lroundf(value); }

static int32_t max_i(int32_t a, int32_t b) { return a > b ? a : b; }

float h2_g99_clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

float h2_g99_ease_in(float t) {
  t = h2_g99_clamp01(t);
  return t * t * t;
}

float h2_g99_ease_out(float t) {
  t = 1.0f - h2_g99_clamp01(t);
  return 1.0f - t * t * t;
}

/* A critically damped spring, normalized to end exactly on the item.
 * A finite endpoint prevents a released wheel remaining between two slots. */
float h2_g99_ease_snap(float t) {
  t = h2_g99_clamp01(t);
  if (t == 0.0f || t == 1.0f) return t;
  return (1.0f - (1.0f + 6.0f * t) * expf(-6.0f * t)) /
         (1.0f - 7.0f * expf(-6.0f));
}

float h2_g99_wheel_follow(float current, float target, uint32_t elapsed_ms) {
  if (!isfinite(current)) current = 0.0f;
  if (!isfinite(target)) target = current;
  current = current < -1.0f ? -1.0f : (current > 1.0f ? 1.0f : current);
  target = target < -1.0f ? -1.0f : (target > 1.0f ? 1.0f : target);
  /* Never repay a stalled frame as one large jump. At most 3.6 items/s and
   * 0.18 item per update, independent of screen size and pointer distance. */
  if (elapsed_ms == 0u) return current;
  if (elapsed_ms > 50u) elapsed_ms = 50u;
  const float limit = 3.6f * (float)elapsed_ms / 1000.0f;
  float delta = (target - current) * (1.0f - expf(-(float)elapsed_ms / 45.0f));
  if (delta > limit) delta = limit;
  if (delta < -limit) delta = -limit;
  const float next = current + delta;
  return fabsf(target - next) < 0.001f ? target : next;
}

int h2_g99_wheel_snap(float offset) {
  /* Keep deliberate short flicks: the next magnetic detent takes over
   * after 30% of a slot. A gesture can advance only one item. */
  if (!isfinite(offset) || fabsf(offset) < 0.3f) return 0;
  return offset > 0.0f ? 1 : -1;
}

bool h2_g99_layout_compute(int32_t width, int32_t height,
                           h2_g99_layout_t* out) {
  if (out == NULL || width <= 0 || height <= 0) return false;
  const int32_t short_side = width < height ? width : height;
  const float k = (float)short_side / 240.0f;
  h2_g99_layout_t l = {0};
  l.width = width;
  l.height = height;
  l.k = k;
  l.pad = round_i(10.0f * k);
  l.sel_x = round_i(0.30f * (float)width);
  l.cy = round_i((float)height / 2.0f);
  l.radius = round_i(0.58f * (float)height);
  l.cx = l.sel_x - l.radius;
  l.panel_x = l.sel_x + round_i(30.0f * k);
  l.panel_y = l.pad;
  l.panel_w = width - l.panel_x - l.pad;
  l.panel_h = height - 2 * l.pad;
  l.panel_inset = round_i(6.0f * k);
  l.font_hero = max_i(22, round_i(18.0f * k));
  l.font_title = max_i(14, round_i(13.0f * k));
  l.font_body = max_i(11, round_i(9.0f * k));
  l.font_small = max_i(10, round_i(7.5f * k));
  l.row_h = round_i(24.0f * k);
  if (!h2_g99_brand_layout(width, height, &l.brand)) return false;
  *out = l;
  return true;
}

float h2_g99_slot_base_size(h2_g99_wheel_kind_t kind, int tier) {
  if (tier < 0 || tier > 2) return 0.0f;
  return kind == H2_G99_WHEEL_THUMBS ? kThumbSizes[tier] : kIconSizes[tier];
}

float h2_g99_slot_base_opacity(int tier) {
  return tier < 0 || tier > 2 ? 0.0f : kOpacities[tier];
}

float h2_g99_wheel_step(const h2_g99_layout_t* layout,
                        h2_g99_wheel_kind_t kind) {
  if (layout == NULL || layout->radius <= 0) return 0.0f;
  const float edge = h2_g99_slot_base_size(kind, 2) * layout->k / 2.0f;
  float ratio =
      ((float)(layout->cy - layout->pad) - edge) / (float)layout->radius;
  if (ratio > 0.98f) ratio = 0.98f;
  if (ratio < 0.0f) ratio = 0.0f;
  return asinf(ratio) / 2.0f;
}

static void place(const h2_g99_layout_t* layout, float angle, float size,
                  float opacity, float glow, h2_g99_slot_t* out) {
  out->x = (float)layout->cx + (float)layout->radius * cosf(angle);
  out->y = (float)layout->cy + (float)layout->radius * sinf(angle);
  out->size = size;
  out->opacity = opacity;
  out->tilt_deg = angle * (180.0f / 3.14159265f) * 0.6f;
  out->glow = glow;
}

void h2_g99_wheel_slot(const h2_g99_layout_t* layout, h2_g99_wheel_kind_t kind,
                       float d, h2_g99_slot_t* out) {
  if (layout == NULL || out == NULL) return;
  const float a = fabsf(d);
  float size = 0.0f;
  float opacity = 0.0f;
  if (a < 3.0f) {
    const int tier = (int)floorf(a);
    const float t = a - (float)tier;
    const float s0 = h2_g99_slot_base_size(kind, tier);
    const float o0 = h2_g99_slot_base_opacity(tier);
    const float s1 = tier < 2 ? h2_g99_slot_base_size(kind, tier + 1)
                              : h2_g99_slot_base_size(kind, 2) * 0.7f;
    const float o1 = tier < 2 ? h2_g99_slot_base_opacity(tier + 1) : 0.0f;
    size = (s0 + (s1 - s0) * t) * layout->k;
    opacity = o0 + (o1 - o0) * t;
  }
  /* The nearest slot is at most half an item away. Retain at least
   * half its glow at that boundary, so selection never disappears. */
  float glow = 1.0f - a;
  if (glow < 0.0f) glow = 0.0f;
  place(layout, d * h2_g99_wheel_step(layout, kind), size, opacity, glow, out);
}

void h2_g99_boot_slot(const h2_g99_layout_t* layout, int d, float unfold,
                      h2_g99_slot_t* out) {
  if (layout == NULL || out == NULL) return;
  const int tier = d < 0 ? -d : d;
  unfold = h2_g99_clamp01(unfold);
  const float size = h2_g99_slot_base_size(H2_G99_WHEEL_ICONS, tier) *
                     layout->k * (0.6f + 0.4f * unfold);
  const float step = h2_g99_wheel_step(layout, H2_G99_WHEEL_ICONS);
  place(layout, (float)d * step * unfold, size,
        h2_g99_slot_base_opacity(tier) * unfold, d == 0 ? unfold : 0.0f, out);
}

void h2_g99_page_frame(const h2_g99_layout_t* layout, float p,
                       h2_g99_page_frame_t* out) {
  if (layout == NULL || out == NULL) return;
  const float dx = -((float)layout->sel_x + 30.0f * layout->k);
  const float e1 = h2_g99_clamp01(p / 0.45f);
  const float e2 = h2_g99_clamp01((p - 0.35f) / 0.45f);
  const float f1 = h2_g99_clamp01(p / 0.3f);
  const float f2 = h2_g99_clamp01((p - 0.6f) / 0.4f);
  out->old_wheel_dx = dx * h2_g99_ease_in(e1);
  out->old_wheel_opacity = 1.0f - e1;
  out->old_info_opacity = 1.0f - f1;
  out->new_wheel_dx = dx * (1.0f - h2_g99_ease_out(e2));
  out->new_wheel_opacity = e2;
  out->new_info_opacity = f2;
}

void h2_g99_boot_frame(uint32_t elapsed_ms, h2_g99_boot_frame_t* out) {
  if (out == NULL) return;
  const float t = (float)elapsed_ms;
  out->brand_opacity = 1.0f - h2_g99_clamp01(t / 300.0f);
  out->background_opacity = h2_g99_ease_out((t - 200.0f) / 400.0f);
  out->unfold = h2_g99_ease_out((t - 350.0f) / 350.0f);
  out->info_opacity = h2_g99_clamp01((t - 600.0f) / 300.0f);
}

void h2_g99_wheel_bounds(const h2_g99_layout_t* layout,
                         h2_g99_wheel_kind_t kind, uint32_t count,
                         h2_g99_rect_t* out) {
  if (out == NULL) return;
  *out = (h2_g99_rect_t){0};
  if (layout == NULL || count == 0u) return;
  int32_t lo = 0;
  int32_t hi = 0;
  h2_g99_wheel_span(count, &lo, &hi);
  float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
  bool any = false;
  for (int32_t d = lo; d <= hi; ++d) {
    h2_g99_slot_t slot;
    h2_g99_wheel_slot(layout, kind, (float)d, &slot);
    if (slot.size <= 0.0f || slot.opacity <= 0.0f) continue;
    const float a = slot.tilt_deg * (3.14159265f / 180.0f);
    const float half = slot.size / 2.0f * (fabsf(cosf(a)) + fabsf(sinf(a)));
    if (!any || slot.x - half < x1) x1 = slot.x - half;
    if (!any || slot.y - half < y1) y1 = slot.y - half;
    if (!any || slot.x + half > x2) x2 = slot.x + half;
    if (!any || slot.y + half > y2) y2 = slot.y + half;
    any = true;
  }
  if (!any) return;
  *out = (h2_g99_rect_t){(int32_t)floorf(x1), (int32_t)floorf(y1),
                         (int32_t)ceilf(x2), (int32_t)ceilf(y2)};
}

int32_t h2_g99_layout_rows(const h2_g99_layout_t* layout, int32_t top,
                           int32_t bottom) {
  if (layout == NULL || layout->row_h <= 0 || bottom <= top) return 1;
  const int32_t rows = (bottom - top) / layout->row_h;
  return rows < 1 ? 1 : rows;
}

void h2_g99_wheel_span(uint32_t count, int32_t* out_lo, int32_t* out_hi) {
  int32_t lo = -2;
  int32_t hi = 2;
  if (count == 0u) {
    lo = 1;
    hi = 0;
  } else if (count < 5u) {
    lo = -(int32_t)((count - 1u) / 2u);
    hi = (int32_t)(count / 2u);
  }
  if (out_lo != NULL) *out_lo = lo;
  if (out_hi != NULL) *out_hi = hi;
}
