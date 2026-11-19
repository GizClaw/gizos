#include "h2_buffet.h"
#include "buffet_render.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  bf_game_t game;
  uint16_t* frame;
  uint16_t* ground;
  uint16_t* previous;
  h2_pal_touch_info_t touch;
  bool first, dining, pressed;
  float press_x, press_y;
  unsigned frames;
  uint64_t stat_start;
} bf_session_t;

static int present(h2_runtime_t* rt, bf_session_t* s) {
  bool dining = s->game.phase == BF_DINING;
  if (s->first || s->dining != dining) {
    bf_paint_ground((bf_canvas_t){s->ground, s->game.en}, dining);
    s->dining = dining;
  }
  memcpy(s->frame, s->ground, 800 * 480 * sizeof(uint16_t));
  bf_paint_scene((bf_canvas_t){s->frame, s->game.en}, &s->game);
  /* One transfer for the changed row range, including the first and last
   * moving sprite. Identical title/result frames cost no PAL submission. */
  int first = 0, last = 479;
  if (!s->first) {
    while (first <= last &&
           memcmp(s->frame + 800 * first, s->previous + 800 * first, 1600) == 0)
      first++;
    while (last >= first &&
           memcmp(s->frame + 800 * last, s->previous + 800 * last, 1600) == 0)
      last--;
  }
  if (first <= last) {
    h2_display_rect_t rect = {0, first, 800, last - first + 1};
    int rc =
        h2_pal_display_draw_bitmap(rt->display, &rect, s->frame + first * 800,
                                   1600, H2_DISPLAY_PIXEL_RGB565);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_display_present(rt->display);
    if (rc != H2_PAL_OK) return rc;
    uint16_t* swap = s->frame;
    s->frame = s->previous;
    s->previous = swap;
  }
  s->first = false;
  return H2_PAL_OK;
}
static int touches(h2_runtime_t* rt, bf_session_t* s, bool active) {
  for (unsigned i = 0; i < 64; i++) {
    h2_pal_touch_event_t ev;
    int rc = h2_pal_touch_poll_event(rt->touch, &ev);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    float x = (float)ev.x * 800 / s->touch.width,
          y = (float)ev.y * 480 / s->touch.height;
    if (ev.kind == H2_PAL_TOUCH_EVENT_DOWN) {
      s->pressed = active;
      s->press_x = x;
      s->press_y = y;
    }
    float dx = x - s->press_x, dy = y - s->press_y;
    if (dx * dx + dy * dy > 14.f * 14.f) s->pressed = false;
    if (ev.kind == H2_PAL_TOUCH_EVENT_UP) {
      if (active && s->pressed) bf_tap(&s->game, x, y);
      s->pressed = false;
    }
  }
  return H2_PAL_OK;
}
static int wait_frame(h2_runtime_t* rt, uint64_t deadline) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  for (;;) {
    for (;;) {
      h2_runtime_event_t event = {.payload = payload,
                                  .payload_capacity = sizeof(payload)};
      int rc = h2_runtime_poll_event(rt, &event);
      if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) break;
      if (rc != H2_PAL_OK) return rc;
    }
    uint64_t now = 0;
    int rc = h2_pal_time_get_monotonic_ms(rt->time, &now);
    if (rc != H2_PAL_OK) return rc;
    if (now >= deadline) return H2_PAL_OK;
    rc = h2_runtime_wait_notify(rt, (uint32_t)(deadline - now));
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) return rc;
  }
}
static int run(h2_runtime_t* rt, bf_session_t* s,
               const h2_buffet_config_t* cfg) {
  h2_display_info_t display;
  int rc = h2_pal_display_get_info(rt->display, &display);
  if (rc != H2_PAL_OK) return rc;
  if (display.native_format != H2_DISPLAY_PIXEL_RGB565 ||
      display.width != 800 || display.height != 480)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_pal_touch_get_info(rt->touch, &s->touch);
  if (rc != H2_PAL_OK) return rc;
  if (!s->touch.width || !s->touch.height) return H2_PAL_ERR_UNSUPPORTED;
  const size_t size = 800 * 480 * sizeof(uint16_t);
  s->frame = h2_pal_mem_alloc(rt->mem, size);
  s->ground = h2_pal_mem_alloc(rt->mem, size);
  s->previous = h2_pal_mem_alloc(rt->mem, size);
  if (!s->frame || !s->ground || !s->previous) return H2_PAL_ERR_NO_MEMORY;
  bf_init(&s->game, cfg && cfg->keyboard);
  s->first = true;
  bool inactive = false;
  uint64_t last = 0;
  rc = h2_pal_time_get_monotonic_ms(rt->time, &last);
  if (rc != H2_PAL_OK) return rc;
  s->stat_start = last;
  for (;;) {
    h2_buffet_input_t input = {0};
    if (cfg && cfg->poll) cfg->poll(cfg->user, &input);
    if (input.close) return H2_PAL_OK;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(rt->time, &now);
    if (rc != H2_PAL_OK) return rc;
    bf_phase_t phase = s->game.phase;
    int cover = s->game.cover;
    bool en = s->game.en, tasting = s->game.tasting;
    if (!input.inactive && !inactive && now >= last) {
      uint64_t dt = now - last;
      bf_step(&s->game, (int)(dt > INT_MAX ? INT_MAX : dt), input.dx, input.dy);
    }
    if (input.inactive) s->pressed = false;
    /* Input belongs to the present instant. A delayed touch cannot rescue a
     * round that closed, or retroactively extend how long a bite was red. */
    bool accept = !input.inactive && phase == s->game.phase;
    if (accept) bf_action(&s->game, input.actions);
    rc = touches(rt, s, accept);
    if (rc != H2_PAL_OK) return rc;
    if (s->first || s->game.phase != phase || cover != s->game.cover ||
        en != s->game.en || tasting != s->game.tasting ||
        (s->game.phase == BF_DINING && !input.inactive)) {
      rc = present(rt, s);
      if (rc != H2_PAL_OK) return rc;
      s->frames++;
    }
    if (s->game.phase != phase) {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "phase=%d elapsed_ms=%d eaten=%d value=%d purse=%d",
               s->game.phase, s->game.elapsed_ms, s->game.eaten, s->game.value,
               s->game.purse);
      (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
    }
    if (now - s->stat_start >= 10000) {
      if (s->game.phase == BF_DINING) {
        char msg[80];
        snprintf(msg, sizeof(msg), "frames=%u interval_ms=%llu", s->frames,
                 (unsigned long long)(now - s->stat_start));
        (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
      }
      s->stat_start = now;
      s->frames = 0;
    }
    last = now;
    inactive = input.inactive;
    rc = wait_frame(rt, now + 33);
    if (rc != H2_PAL_OK) return rc;
  }
}
h2_pal_result_t h2_buffet_run(h2_runtime_t* rt, const h2_buffet_config_t* cfg) {
  if (!rt) return H2_PAL_ERR_INVALID_ARG;
  bf_session_t* s = h2_pal_mem_alloc(rt->mem, sizeof(*s));
  if (!s) return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  int rc = h2_pal_display_open(rt->display);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_touch_open(rt->touch);
    if (rc == H2_PAL_OK) {
      rc = run(rt, s, cfg);
      int closed = h2_pal_touch_close(rt->touch);
      if (rc == H2_PAL_OK) rc = closed;
    }
    int closed = h2_pal_display_close(rt->display);
    if (rc == H2_PAL_OK) rc = closed;
  }
  h2_pal_mem_free(rt->mem, s->frame);
  h2_pal_mem_free(rt->mem, s->ground);
  h2_pal_mem_free(rt->mem, s->previous);
  h2_pal_mem_free(rt->mem, s);
  return rc;
}
