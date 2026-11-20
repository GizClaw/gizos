#include "h2_buffet.h"
#include "buffet_render.h"
#include "buffet_cards.h"
#include "buffet_audio.h"
#include "buffet_checkpoint.h"
#include "buffet_damage.h"
#include "buffet_scale.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
  WIDTH = BF_FRAME_W,
  HEIGHT = BF_FRAME_H,
  STRIDE = WIDTH * sizeof(uint16_t)
};
typedef struct {
  bf_game_t game;
  bf_audio_t audio;
  uint16_t* frame;
  uint16_t* ground;
  uint16_t* previous;
  uint8_t* heights;
  h2_pal_touch_info_t touch;
  bool first, dining;
  unsigned frames, walking_frames, pending_cards;
  unsigned lcd_bytes, lcd_rects;
  unsigned stick_down, stick_out, stick_up, stick_up_out, stick_eat;
  unsigned intervals[64], interval_count, max_interval_ms;
  bool previous_live;
  uint64_t stat_start, pref_retry_ms;
} bf_session_t;

static int present(h2_runtime_t* rt, bf_session_t* s,
                   const h2_buffet_config_t* cfg) {
  bool dining = s->game.phase == BF_DINING;
  bool moving_room = BF_FRAME_W <= 480 && !s->first && dining && s->dining &&
                     s->game.meal_menu == BF_LIVE;
  if (s->first || s->dining != dining) {
    bf_checkpoint(cfg, H2_BUFFET_STAGE_GROUND);
    bf_paint_ground(
        (bf_canvas_t){
            .pixels = s->ground, .en = s->game.en, .heights = s->heights},
        dining);
    s->dining = dining;
  }
  bf_checkpoint(cfg, H2_BUFFET_STAGE_COPY);
  memcpy(s->frame, s->ground, STRIDE * HEIGHT);
  bf_checkpoint(cfg, H2_BUFFET_STAGE_SCENE);
  bf_paint_scene(
      (bf_canvas_t){
          .pixels = s->frame, .en = s->game.en, .heights = s->heights},
      &s->game);
  bf_checkpoint(cfg, H2_BUFFET_STAGE_COMPARE);
  bf_rect_t changed[BF_DAMAGE_ZONES];
  unsigned count = 0;
  if (moving_room) {
    count = bf_damage_rects(s->frame, s->previous, changed);
  } else {
    int first = 0, last = HEIGHT - 1;
    if (!s->first) {
      while (first <= last && memcmp(s->frame + WIDTH * first,
                                     s->previous + WIDTH * first, STRIDE) == 0)
        first++;
      while (last >= first && memcmp(s->frame + WIDTH * last,
                                     s->previous + WIDTH * last, STRIDE) == 0)
        last--;
    }
    if (first <= last)
      changed[count++] = (bf_rect_t){0, first, WIDTH, last - first + 1};
  }
  if (count) {
    bf_checkpoint(cfg, H2_BUFFET_STAGE_BITMAP);
    if (s->first && BF_LCD_PAD > 0) {
      /* The panel keeps pixels outside the game's viewport. The previous
       * frame is free scratch until the first successful presentation. */
      _Static_assert(BF_LCD_PAD <= BF_FRAME_H, "matte must fit a game frame");
      memset(s->previous, 0, STRIDE * BF_LCD_PAD);
      h2_display_rect_t bar = {0, 0, WIDTH, BF_LCD_PAD};
      int rc = h2_pal_display_draw_bitmap(rt->display, &bar, s->previous,
                                          STRIDE, H2_DISPLAY_PIXEL_RGB565);
      if (rc != H2_PAL_OK) return rc;
      bar.y = BF_LCD_PAD + HEIGHT;
      rc = h2_pal_display_draw_bitmap(rt->display, &bar, s->previous, STRIDE,
                                      H2_DISPLAY_PIXEL_RGB565);
      if (rc != H2_PAL_OK) return rc;
    }
    for (unsigned i = 0; i < count; ++i) {
      bf_rect_t r = changed[i];
      h2_display_rect_t rect = {r.x, r.y + BF_LCD_PAD, r.w, r.h};
      int rc = h2_pal_display_draw_bitmap(rt->display, &rect,
                                          s->frame + r.y * WIDTH + r.x, STRIDE,
                                          H2_DISPLAY_PIXEL_RGB565);
      if (rc != H2_PAL_OK) return rc;
      s->lcd_bytes += (unsigned)(r.w * r.h) * sizeof(uint16_t);
      ++s->lcd_rects;
    }
    bf_checkpoint(cfg, H2_BUFFET_STAGE_PRESENT);
    int rc = h2_pal_display_present(rt->display);
    if (rc != H2_PAL_OK) return rc;
    uint16_t* swap = s->frame;
    s->frame = s->previous;
    s->previous = swap;
  }
  s->first = false;
  return H2_PAL_OK;
}
static int touches(h2_runtime_t* rt, bf_session_t* s, bool active) {
  bf_phase_t phase = s->game.phase;
  bf_meal_menu_t menu = s->game.meal_menu;
  for (unsigned i = 0; i < 64; i++) {
    h2_pal_touch_event_t ev;
    int rc = h2_pal_touch_poll_event(rt->touch, &ev);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    float x = (float)ev.x * H2_BUFFET_WIDTH / s->touch.width;
    float y = ((float)ev.y * (HEIGHT + 2 * BF_LCD_PAD) / s->touch.height -
               BF_LCD_PAD) *
              H2_BUFFET_HEIGHT / HEIGHT;
    if (active) {
      bool stick_owner = s->game.controls.stick_held &&
                         s->game.controls.stick_id == ev.pointer_id;
      bool while_sticking = s->game.controls.stick_held && !stick_owner;
      int eaten = s->game.eaten;
      bf_pointer(&s->game, ev.kind, ev.pointer_id, x, y);
      if (phase == BF_DINING && menu == BF_LIVE) {
        if (ev.kind == H2_PAL_TOUCH_EVENT_DOWN && !stick_owner &&
            s->game.controls.stick_held &&
            s->game.controls.stick_id == ev.pointer_id)
          ++s->stick_down;
        if (stick_owner && !bf_in(bf_stick_bounds, x, y)) {
          if (ev.kind == H2_PAL_TOUCH_EVENT_MOVE) ++s->stick_out;
          if (ev.kind == H2_PAL_TOUCH_EVENT_UP) ++s->stick_up_out;
        }
        if (stick_owner && ev.kind == H2_PAL_TOUCH_EVENT_UP) ++s->stick_up;
        if (while_sticking && s->game.eaten > eaten) ++s->stick_eat;
      }
      /* Drain the rest of this batch against its original input surface. */
      active = phase == s->game.phase && menu == s->game.meal_menu;
    }
  }
  return H2_PAL_OK;
}
static int wait_frame(h2_runtime_t* rt, uint64_t deadline, uint64_t* last_rest,
                      const h2_buffet_config_t* cfg) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  bool waited = false;
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
    if (now >= deadline) {
      if (!waited) bf_checkpoint(cfg, H2_BUFFET_STAGE_LATE);
      /* An over-budget frame can immediately start another render. Give the
       * scheduler a real blocking point even with a full room and active DAC.
       * Rate-limit the cost to one minimum sleep per 100 ms of late frames. */
      if (last_rest && !waited && now - *last_rest >= 100u) {
        *last_rest = now;
        bf_checkpoint(cfg, H2_BUFFET_STAGE_REST);
        rc = h2_pal_time_sleep_ms(rt->time, 1u);
        if (rc == H2_PAL_OK) bf_checkpoint(cfg, H2_BUFFET_STAGE_REST_DONE);
        return rc;
      }
      return H2_PAL_OK;
    }
    waited = true;
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
      display.width != WIDTH || display.height != HEIGHT + 2 * BF_LCD_PAD)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_pal_touch_get_info(rt->touch, &s->touch);
  if (rc != H2_PAL_OK) return rc;
  if (!s->touch.width || !s->touch.height) return H2_PAL_ERR_UNSUPPORTED;
  const size_t size = STRIDE * HEIGHT;
  s->frame = h2_pal_mem_alloc(rt->mem, size);
  s->ground = h2_pal_mem_alloc(rt->mem, size);
  s->previous = h2_pal_mem_alloc(rt->mem, size);
  s->heights = h2_pal_mem_alloc(rt->mem, WIDTH * HEIGHT);
  if (!s->frame || !s->ground || !s->previous || !s->heights)
    return H2_PAL_ERR_NO_MEMORY;
  bf_init(&s->game, cfg && cfg->keyboard);
  bf_audio_init(&s->audio, rt, &s->game, cfg);
  s->game.album.reduced_motion = cfg && cfg->reduced_motion;
  (void)bf_cards_sync(rt->pref, &s->game.album, &s->pending_cards, cfg);
  s->first = true;
  bool inactive = false;
  uint64_t last = 0;
  rc = h2_pal_time_get_monotonic_ms(rt->time, &last);
  if (rc != H2_PAL_OK) return rc;
  s->stat_start = last;
  uint64_t last_rest = last;
  uint64_t* rest = cfg && cfg->cooperate_on_overrun ? &last_rest : NULL;
  for (;;) {
    bf_checkpoint(cfg,
                  (s->game.phase == BF_DINING && s->game.meal_menu == BF_LIVE)
                      ? H2_BUFFET_STAGE_DINING_FRAME
                      : H2_BUFFET_STAGE_FRAME);
    h2_buffet_input_t input = {0};
    if (cfg && cfg->poll) cfg->poll(cfg->user, &input);
    if (input.close) {
      if (s->pending_cards)
        (void)bf_cards_sync(rt->pref, &s->game.album, &s->pending_cards, cfg);
      return H2_PAL_OK;
    }
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(rt->time, &now);
    if (rc != H2_PAL_OK) return rc;
    bf_phase_t phase = s->game.phase;
    bool live =
        phase == BF_DINING && s->game.meal_menu == BF_LIVE && !input.inactive;
    if (live && s->previous_live && now >= last) {
      uint64_t ms = now - last;
      ++s->intervals[ms < 252u ? (ms + 3u) / 4u : 63u];
      ++s->interval_count;
      if (ms > s->max_interval_ms)
        s->max_interval_ms = ms <= UINT_MAX ? (unsigned)ms : UINT_MAX;
    }
    s->previous_live = live;
    bf_meal_menu_t menu = s->game.meal_menu;
    bool leave_selected = s->game.leave_selected;
    bf_sound_state_t sound = bf_sound_state(&s->game);
    bf_album_t album = s->game.album;
    int cover = s->game.cover;
    bool en = s->game.en, tasting = s->game.tasting;
    bf_checkpoint(cfg, H2_BUFFET_STAGE_STEP);
    if (!input.inactive && !inactive && now >= last) {
      uint64_t dt = now - last;
      bf_step(&s->game, (int)(dt > INT_MAX ? INT_MAX : dt), input.dx, input.dy);
    }
    if (input.inactive) bf_reset_controls(&s->game);
    /* Input belongs to the present instant. A delayed touch cannot rescue a
     * round that closed, or retroactively extend how long a bite was red. */
    bool accept = !input.inactive && phase == s->game.phase;
    if (accept) bf_action(&s->game, input.actions);
    if (album.open != s->game.album.open ||
        album.inspecting != s->game.album.inspecting)
      bf_reset_controls(&s->game);
    bf_checkpoint(cfg, H2_BUFFET_STAGE_TOUCH);
    rc = touches(rt, s,
                 accept && phase == s->game.phase && menu == s->game.meal_menu);
    if (rc != H2_PAL_OK) return rc;
    bool changed_menu = menu != s->game.meal_menu;
    if (cfg && cfg->release_keys && (changed_menu || phase != s->game.phase))
      cfg->release_keys(cfg->user);
    bool settled = phase == BF_DINING && (s->game.phase == BF_CLOSING ||
                                          s->game.phase == BF_HOSPITAL);
    if (settled) s->pending_cards |= s->game.album.awaiting;
    if (settled || (s->pending_cards && now >= s->pref_retry_ms)) {
      bf_checkpoint(cfg, H2_BUFFET_STAGE_PREF);
      int saved =
          bf_cards_sync(rt->pref, &s->game.album, &s->pending_cards, cfg);
      s->pref_retry_ms = now + (saved == H2_PAL_ERR_WOULD_BLOCK ? 50u : 2400u);
    }
    bf_checkpoint(cfg, H2_BUFFET_STAGE_AUDIO);
    bf_audio_update(&s->audio, sound, &s->game, now, input.inactive);
    bool first = s->first;
    if (s->first || changed_menu || leave_selected != s->game.leave_selected ||
        sound.muted != s->game.muted || s->game.phase != phase ||
        cover != s->game.cover || album.open != s->game.album.open ||
        album.this_meal != s->game.album.this_meal ||
        album.selected != s->game.album.selected ||
        album.inspecting != s->game.album.inspecting ||
        album.inspect_ms != s->game.album.inspect_ms ||
        album.reveal_ms != s->game.album.reveal_ms ||
        album.fresh != s->game.album.fresh ||
        album.unsaved != s->game.album.unsaved || en != s->game.en ||
        tasting != s->game.tasting ||
        (s->game.phase == BF_DINING && s->game.meal_menu == BF_LIVE &&
         !input.inactive)) {
      rc = present(rt, s, cfg);
      if (rc != H2_PAL_OK) return rc;
      if (first && cfg && cfg->ready) {
        rc = cfg->ready(cfg->user);
        if (rc != H2_PAL_OK) return rc;
      }
      s->frames++;
      if (s->game.phase == BF_DINING && s->game.player.walking)
        ++s->walking_frames;
    }
    bf_checkpoint(cfg, H2_BUFFET_STAGE_REPORT);
    if (s->game.phase != phase) {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "phase=%d elapsed_ms=%d eaten=%d value=%d purse=%d muted=%d",
               s->game.phase, s->game.elapsed_ms, s->game.eaten, s->game.value,
               s->game.purse, (int)s->game.muted);
      (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
    }
    if (changed_menu) {
      char msg[128];
      snprintf(
          msg, sizeof(msg),
          "menu=%d elapsed_ms=%d eaten=%d value=%d fullness=%d red_ms=%d served=%u",
          s->game.meal_menu, s->game.elapsed_ms, s->game.eaten, s->game.value,
          s->game.fullness_milli, s->game.red_ms, s->game.served);
      (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
    }
    bool reset_stats = changed_menu || phase != s->game.phase;
    if (!reset_stats && now - s->stat_start >= 10000) {
      if (s->game.phase == BF_DINING && s->game.meal_menu == BF_LIVE) {
        unsigned rank = (s->interval_count * 95u + 99u) / 100u;
        unsigned bucket = 0;
        while (bucket < 63 && rank > s->intervals[bucket])
          rank -= s->intervals[bucket++];
        char msg[160];
        snprintf(
            msg, sizeof(msg),
            "frames=%u interval_ms=%llu walking=%u eaten=%d p95_ms=%u max_ms=%u lcd_bytes=%u rects=%u",
            s->frames, (unsigned long long)(now - s->stat_start),
            s->walking_frames, s->game.eaten,
            bucket < 63 ? bucket * 4u : s->max_interval_ms, s->max_interval_ms,
            s->lcd_bytes, s->lcd_rects);
        (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
        if (cfg && cfg->report_profile) {
          cfg->report_profile(cfg->user);
          if (s->stick_down || s->stick_out || s->stick_up || s->stick_eat) {
            snprintf(msg, sizeof(msg),
                     "stick down=%u move_out=%u up=%u up_out=%u eat=%u",
                     s->stick_down, s->stick_out, s->stick_up, s->stick_up_out,
                     s->stick_eat);
            (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "buffet", msg);
          }
        }
      }
      reset_stats = true;
    }
    if (reset_stats) {
      s->stat_start = now;
      s->frames = s->walking_frames = s->lcd_bytes = s->lcd_rects = 0;
      s->stick_down = s->stick_out = s->stick_up = s->stick_up_out = 0;
      s->stick_eat = 0;
      s->interval_count = s->max_interval_ms = 0;
      memset(s->intervals, 0, sizeof(s->intervals));
    }
    last = now;
    inactive = input.inactive;
    bf_checkpoint(cfg, H2_BUFFET_STAGE_WAIT);
    rc = wait_frame(rt, now + 33, rest, cfg);
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
      if (rc != H2_PAL_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "run failed rc=%d", rc);
        (void)h2_pal_log_write(rt->log, H2_PAL_LOG_ERROR, "buffet", msg);
      }
      bf_checkpoint(cfg, H2_BUFFET_STAGE_CLEANUP);
      int closed = h2_pal_touch_close(rt->touch);
      if (rc == H2_PAL_OK) rc = closed;
    }
    int closed = h2_pal_display_close(rt->display);
    if (rc == H2_PAL_OK) rc = closed;
  }
  int audio_closed = bf_audio_close(&s->audio);
  if (rc == H2_PAL_OK) rc = audio_closed;
  h2_pal_mem_free(rt->mem, s->frame);
  h2_pal_mem_free(rt->mem, s->ground);
  h2_pal_mem_free(rt->mem, s->previous);
  h2_pal_mem_free(rt->mem, s->heights);
  h2_pal_mem_free(rt->mem, s);
  return rc;
}
