#include "h2_g99_screen.h"

#include <string.h>

/* Square tiles keep the source reads and the frame writes cache local. */
#define TILE 16

static void clear_dirty(h2_g99_screen_t* s) { s->dirty_count = 0u; }

static h2_display_rect_t bounds(const h2_display_rect_t* a,
                                const h2_display_rect_t* b) {
  const int32_t x = a->x < b->x ? a->x : b->x;
  const int32_t y = a->y < b->y ? a->y : b->y;
  const int32_t right =
      a->x + a->width > b->x + b->width ? a->x + a->width : b->x + b->width;
  const int32_t bottom =
      a->y + a->height > b->y + b->height ? a->y + a->height : b->y + b->height;
  return (h2_display_rect_t){x, y, right - x, bottom - y};
}

static uint64_t area(const h2_display_rect_t* rect) {
  return (uint64_t)rect->width * (uint64_t)rect->height;
}

static void mark_dirty(h2_g99_screen_t* s, h2_display_rect_t rect) {
  for (uint8_t i = 0u; i < s->dirty_count;) {
    const h2_display_rect_t merged = bounds(&s->dirty[i], &rect);
    if (area(&merged) <= area(&s->dirty[i]) + area(&rect)) {
      rect = merged;
      s->dirty[i] = s->dirty[--s->dirty_count];
      i = 0u;
    } else {
      ++i;
    }
  }
  const size_t capacity = sizeof(s->dirty) / sizeof(s->dirty[0]);
  if (s->dirty_count < capacity) {
    s->dirty[s->dirty_count++] = rect;
    return;
  }
  /* When the list is full, grow the cheapest existing region. Every pending
   * pixel stays covered without an unconditional full-screen fallback. */
  uint8_t nearest = 0u;
  uint64_t added = UINT64_MAX;
  for (uint8_t i = 0u; i < s->dirty_count; ++i) {
    const h2_display_rect_t merged = bounds(&s->dirty[i], &rect);
    const uint64_t cost = area(&merged) - area(&s->dirty[i]);
    if (cost < added) {
      nearest = i;
      added = cost;
    }
  }
  s->dirty[nearest] = bounds(&s->dirty[nearest], &rect);
}

/* ---------- display ---------- */

static int panel_size(h2_g99_screen_t* s) {
  if (s->panel_width > 0 && s->panel_height > 0) return H2_PAL_OK;
  h2_display_info_t info;
  memset(&info, 0, sizeof(info));
  const int rc = h2_pal_display_get_info(s->board_display, &info);
  if (rc != H2_PAL_OK) return rc;
  if (info.width <= 0 || info.height <= 0) return H2_PAL_ERR_INVALID_STATE;
  s->panel_width = info.width;
  s->panel_height = info.height;
  return H2_PAL_OK;
}

/* The first open allocates the frame. It starts black, like the board's own
 * shadow, and keeps its content from frame to frame. */
static int display_open(void* user) {
  h2_g99_screen_t* s = user;
  int rc = h2_pal_display_open(s->board_display);
  if (rc != H2_PAL_OK) return rc;
  if (s->rotation == H2_G99_SCREEN_ROTATION_NONE) return H2_PAL_OK;
  rc = panel_size(s);
  if (rc == H2_PAL_OK && s->frame == NULL) {
    if ((size_t)s->panel_width >
        SIZE_MAX / sizeof(uint16_t) / (size_t)s->panel_height) {
      rc = H2_PAL_ERR_INVALID_ARG;
    } else {
      const size_t bytes =
          (size_t)s->panel_width * (size_t)s->panel_height * sizeof(uint16_t);
      s->frame = h2_pal_mem_alloc(s->mem, bytes);
      if (s->frame == NULL)
        rc = H2_PAL_ERR_NO_MEMORY;
      else
        memset(s->frame, 0, bytes);
    }
  }
  if (rc != H2_PAL_OK) {
    (void)h2_pal_display_close(s->board_display);
    return rc;
  }
  clear_dirty(s);
  s->frame_valid = false;
  return H2_PAL_OK;
}

static int display_get_info(void* user, h2_display_info_t* info) {
  h2_g99_screen_t* s = user;
  if (info == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int rc = h2_pal_display_get_info(s->board_display, info);
  if (rc != H2_PAL_OK) return rc;
  if (s->rotation == H2_G99_SCREEN_ROTATION_NONE) return H2_PAL_OK;
  const int width = info->width;
  info->width = info->height;
  info->height = width;
  return H2_PAL_OK;
}

/* Logical column x lands on panel row H - 1 - x and logical row y on panel
 * column y. Only the frame changes here; present() sends it. */
static int display_draw_bitmap(void* user, const h2_display_rect_t* rect,
                               const void* pixels, size_t stride_bytes,
                               h2_display_pixel_format_t format) {
  h2_g99_screen_t* s = user;
  if (rect == NULL || pixels == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s->rotation == H2_G99_SCREEN_ROTATION_NONE)
    return h2_pal_display_draw_bitmap(s->board_display, rect, pixels,
                                      stride_bytes, format);
  if (format != H2_DISPLAY_PIXEL_RGB565) return H2_PAL_ERR_UNSUPPORTED;
  if (s->frame == NULL) return H2_PAL_ERR_INVALID_STATE;
  const int32_t logical_w = s->panel_height;
  const int32_t logical_h = s->panel_width;
  if (rect->width <= 0 || rect->height <= 0 || rect->x < 0 || rect->y < 0 ||
      rect->x > logical_w - rect->width || rect->y > logical_h - rect->height ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t) ||
      (rect->height > 1 &&
       stride_bytes > (SIZE_MAX - (size_t)rect->width * sizeof(uint16_t)) /
                          (size_t)(rect->height - 1)))
    return H2_PAL_ERR_INVALID_ARG;
  const uint8_t* src = pixels;
  const size_t pitch = (size_t)s->panel_width;
  const bool compare = s->frame_valid;
  uint16_t tile[TILE * TILE];
  for (int32_t ty = 0; ty < rect->height; ty += TILE) {
    const int32_t th = rect->height - ty < TILE ? rect->height - ty : TILE;
    for (int32_t tx = 0; tx < rect->width; tx += TILE) {
      const int32_t tw = rect->width - tx < TILE ? rect->width - tx : TILE;
      /* Bulk-copy rows into aligned storage. Besides keeping the transpose
       * cache local, this avoids a two-byte memcpy through the PSRAM stack
       * for every pixel on strict-alignment RISC-V compilers. */
      for (int32_t r = 0; r < th; ++r)
        memcpy(&tile[r * TILE],
               src + (size_t)(ty + r) * stride_bytes +
                   (size_t)tx * sizeof(uint16_t),
               (size_t)tw * sizeof(uint16_t));
      int32_t x1 = tw, y1 = th, x2 = -1, y2 = -1;
      for (int32_t c = tx; c < tx + tw; ++c) {
        uint16_t* dst =
            &s->frame[(size_t)(s->panel_height - 1 - rect->x - c) * pitch +
                      (size_t)(rect->y + ty)];
        for (int32_t r = 0; r < th; ++r) {
          const uint16_t pixel = tile[r * TILE + c - tx];
          if (!compare || dst[r] != pixel) {
            dst[r] = pixel;
            const int32_t x = c - tx;
            if (x < x1) x1 = x;
            if (x > x2) x2 = x;
            if (r < y1) y1 = r;
            if (r > y2) y2 = r;
          }
        }
      }
      /* LVGL invalidation includes old object bounds and transparent pixels.
       * Keep those unchanged pixels out of the physical transfer. */
      if (compare && x2 >= x1)
        mark_dirty(s,
                   (h2_display_rect_t){rect->y + ty + y1,
                                       s->panel_height - 1 - rect->x - tx - x2,
                                       y2 - y1 + 1, x2 - x1 + 1});
    }
  }
  if (!s->frame_valid)
    mark_dirty(
        s, (h2_display_rect_t){rect->y, s->panel_height - rect->x - rect->width,
                               rect->height, rect->width});
  return H2_PAL_OK;
}

/* Preserve rotated dirty bounds and the full-frame source stride. Retain
 * pending regions on any draw/present failure so a retry can finish them. */
static int display_present(void* user) {
  h2_g99_screen_t* s = user;
  bool full_frame = false;
  for (uint8_t i = 0u; i < s->dirty_count; ++i) {
    const h2_display_rect_t* rect = &s->dirty[i];
    const int rc = h2_pal_display_draw_bitmap(
        s->board_display, rect,
        &s->frame[(size_t)rect->y * (size_t)s->panel_width + (size_t)rect->x],
        (size_t)s->panel_width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    if (rc != H2_PAL_OK) return rc;
    if (rect->x == 0 && rect->y == 0 && rect->width == s->panel_width &&
        rect->height == s->panel_height)
      full_frame = true;
  }
  const int rc = h2_pal_display_present(s->board_display);
  if (rc == H2_PAL_OK) {
    if (full_frame) s->frame_valid = true;
    clear_dirty(s);
  }
  return rc;
}

static int display_set_brightness(void* user, uint32_t percent) {
  h2_g99_screen_t* s = user;
  return h2_pal_display_set_brightness_percent(s->board_display, percent);
}

static int display_close(void* user) {
  h2_g99_screen_t* s = user;
  clear_dirty(s);
  s->frame_valid = false;
  return h2_pal_display_close(s->board_display);
}

static const h2_pal_display_vtable_t kDisplayVtable = {
    display_open,    display_get_info,       display_draw_bitmap,
    display_present, display_set_brightness, display_close,
};

/* ---------- touch ---------- */

static h2_pal_result_t touch_open(void* user) {
  h2_g99_screen_t* s = user;
  return h2_pal_touch_open(s->board_touch);
}

static h2_pal_result_t touch_get_info(void* user, h2_pal_touch_info_t* out) {
  h2_g99_screen_t* s = user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_result_t rc = h2_pal_touch_get_info(s->board_touch, out);
  if (rc != H2_PAL_OK) return rc;
  if (s->rotation == H2_G99_SCREEN_ROTATION_NONE) return H2_PAL_OK;
  const uint32_t width = out->width;
  out->width = out->height;
  out->height = width;
  return H2_PAL_OK;
}

/* Panel (px, py) is logical (H - 1 - py, px), H being the touch height. */
static h2_pal_result_t touch_poll_event(void* user, h2_pal_touch_event_t* out) {
  h2_g99_screen_t* s = user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s->rotation == H2_G99_SCREEN_ROTATION_NONE)
    return h2_pal_touch_poll_event(s->board_touch, out);
  h2_pal_touch_info_t info;
  memset(&info, 0, sizeof(info));
  h2_pal_result_t rc = h2_pal_touch_get_info(s->board_touch, &info);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_pal_touch_poll_event(s->board_touch, out);
  if (rc != H2_PAL_OK) return rc;
  const int32_t px = out->x;
  out->x = (int32_t)info.height - 1 - out->y;
  out->y = px;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_close(void* user) {
  h2_g99_screen_t* s = user;
  return h2_pal_touch_close(s->board_touch);
}

static const h2_pal_touch_vtable_t kTouchVtable = {
    touch_open, touch_get_info, touch_poll_event, touch_close};

/* ---------- setup ---------- */

h2_pal_result_t h2_g99_screen_init(h2_g99_screen_t* screen,
                                   const h2_pal_display_api_t* board_display,
                                   const h2_pal_touch_api_t* board_touch,
                                   const h2_pal_mem_api_t* mem,
                                   h2_g99_screen_rotation_t rotation) {
  if (screen == NULL ||
      (rotation != H2_G99_SCREEN_ROTATION_NONE &&
       rotation != H2_G99_SCREEN_ROTATION_CCW_90) ||
      (board_display != NULL && rotation != H2_G99_SCREEN_ROTATION_NONE &&
       (mem == NULL || mem->vtable == NULL || mem->vtable->alloc == NULL ||
        mem->vtable->free == NULL)))
    return H2_PAL_ERR_INVALID_ARG;
  memset(screen, 0, sizeof(*screen));
  screen->board_display = board_display;
  screen->board_touch = board_touch;
  screen->mem = mem;
  screen->rotation = rotation;
  screen->display.user = screen;
  screen->display.vtable = &kDisplayVtable;
  screen->touch.user = screen;
  screen->touch.vtable = &kTouchVtable;
  return H2_PAL_OK;
}

void h2_g99_screen_deinit(h2_g99_screen_t* screen) {
  if (screen == NULL || screen->frame == NULL) return;
  h2_pal_mem_free(screen->mem, screen->frame);
  screen->frame = NULL;
}

const h2_pal_display_api_t* h2_g99_screen_display(
    const h2_g99_screen_t* screen) {
  return screen != NULL && screen->board_display != NULL ? &screen->display
                                                         : NULL;
}

const h2_pal_touch_api_t* h2_g99_screen_touch(const h2_g99_screen_t* screen) {
  return screen != NULL && screen->board_touch != NULL ? &screen->touch : NULL;
}
