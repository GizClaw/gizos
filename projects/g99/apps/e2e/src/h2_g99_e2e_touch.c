#include "hal/h2_pal_display.h"
#include "h2_g99_e2e_internal.h"

#include <string.h>

/* ---------- touch source, read by Main ---------- */

static h2_pal_result_t finger_open(void* user) {
  h2_g99_e2e_finger_t* f = user;
  return f->real != NULL ? h2_pal_touch_open(f->real) : H2_PAL_OK;
}

static h2_pal_result_t finger_get_info(void* user, h2_pal_touch_info_t* out) {
  h2_g99_e2e_finger_t* f = user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (f->real != NULL) return h2_pal_touch_get_info(f->real, out);
  /* Without Runtime touch the finger covers the display. */
  h2_display_info_t info;
  memset(&info, 0, sizeof(info));
  const int rc = h2_pal_display_get_info(f->e2e->runtime->display, &info);
  if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
  out->width = (uint32_t)info.width;
  out->height = (uint32_t)info.height;
  return H2_PAL_OK;
}

/* Reports the scripted finger as edges, like a touch controller sampled
 * at this moment: down, a move to wherever it is now, or up. */
static h2_pal_result_t finger_poll(void* user, h2_pal_touch_event_t* out) {
  h2_g99_e2e_finger_t* f = user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_g99_e2e_t* e2e = f->e2e;
  const uint64_t now = h2_g99_e2e_now_ms(e2e);
  h2_g99_e2e_lock(e2e);
  if (!f->scripted) {
    h2_g99_e2e_unlock(e2e);
    return f->real != NULL ? h2_pal_touch_poll_event(f->real, out)
                           : H2_PAL_ERR_WOULD_BLOCK;
  }
  const uint64_t since = f->reads > 0u ? f->last_read_ms : f->begun_ms;
  if (now > since && now - since > f->max_gap_ms)
    f->max_gap_ms = (uint32_t)(now - since);
  f->last_read_ms = now;
  ++f->reads;
  /* A controller model has news once per report; in between a read fails
   * (driver 0.1.2) or finds an empty frame (driver 0.1.1). */
  const bool fresh = f->model == H2_G99_E2E_TOUCH_CLEAN ||
                     now - f->report_ms >= H2_G99_E2E_REPORT_MS;
  if (!fresh && f->model == H2_G99_E2E_TOUCH_FAILED_READS) {
    h2_g99_e2e_unlock(e2e);
    return H2_PAL_ERR_IO;
  }
  if (fresh) f->report_ms = now;
  const bool contact = f->down && fresh;
  h2_pal_result_t rc = H2_PAL_ERR_WOULD_BLOCK;
  if (contact &&
      (!f->reported_down || f->x != f->reported_x || f->y != f->reported_y)) {
    *out = (h2_pal_touch_event_t){
        f->reported_down ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN,
        f->x, f->y};
    f->reported_down = true;
    f->reported_x = f->x;
    f->reported_y = f->y;
    rc = H2_PAL_OK;
  } else if (!contact && f->reported_down) {
    *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, f->reported_x,
                                  f->reported_y};
    f->reported_down = false;
    rc = H2_PAL_OK;
  } else if (!f->down && f->hovering &&
             (!f->hover_reported || f->hover_x != f->hover_reported_x ||
              f->hover_y != f->hover_reported_y)) {
    /* A mouse moving with no button pressed, as the Web platform reports
     * it: a move without contact. */
    *out =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, f->hover_x, f->hover_y};
    f->hover_reported = true;
    f->hover_reported_x = f->hover_x;
    f->hover_reported_y = f->hover_y;
    rc = H2_PAL_OK;
  }
  /* Main has read the finger once a report showed its latest state. */
  if (fresh) f->read_seq = f->state_seq;
  h2_g99_e2e_unlock(e2e);
  return rc;
}

static h2_pal_result_t finger_close(void* user) {
  h2_g99_e2e_finger_t* f = user;
  return f->real != NULL ? h2_pal_touch_close(f->real) : H2_PAL_OK;
}

static const h2_pal_touch_vtable_t kFingerVtable = {
    finger_open, finger_get_info, finger_poll, finger_close};

void h2_g99_e2e_finger_init(h2_g99_e2e_t* e2e, const h2_pal_touch_api_t* real) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  memset(f, 0, sizeof(*f));
  f->e2e = e2e;
  f->real = real;
  f->api.user = f;
  f->api.vtable = &kFingerVtable;
}

/* ---------- scripting, from the suite task ---------- */

/* Takes the touch source over with the finger up and counters cleared. */
static void begin(h2_g99_e2e_t* e2e) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  const uint64_t now = h2_g99_e2e_now_ms(e2e);
  h2_g99_e2e_lock(e2e);
  f->scripted = true;
  f->down = false;
  f->reported_down = false;
  f->hovering = false;
  f->hover_reported = false;
  f->report_ms = 0u;
  f->begun_ms = now;
  f->reads = 0u;
  f->last_read_ms = 0u;
  f->max_gap_ms = 0u;
  ++f->state_seq;
  h2_g99_e2e_unlock(e2e);
}

static void place_hover(h2_g99_e2e_t* e2e, int32_t x, int32_t y) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  h2_g99_e2e_lock(e2e);
  f->hovering = true;
  f->hover_x = x;
  f->hover_y = y;
  ++f->state_seq;
  h2_g99_e2e_unlock(e2e);
}

static void place(h2_g99_e2e_t* e2e, bool down, int32_t x, int32_t y) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  h2_g99_e2e_lock(e2e);
  f->down = down;
  if (down) {
    f->x = x;
    f->y = y;
  }
  ++f->state_seq;
  h2_g99_e2e_unlock(e2e);
}

/* Waits until Main read the latest finger state. */
static h2_pal_result_t wait_read(h2_g99_e2e_t* e2e, const char* what) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_e2e_lock(e2e);
    const bool read = f->read_seq == f->state_seq;
    h2_g99_e2e_unlock(e2e);
    if (read) return H2_PAL_OK;
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (elapsed >= H2_G99_E2E_TOUCH_READ_MS || !h2_g99_e2e_main_running(e2e) ||
        h2_g99_e2e_aborted(e2e)) {
      h2_g99_e2e_fail(e2e, "Main did not read the touch %s within %u ms", what,
                      (unsigned)elapsed);
      return H2_PAL_ERR_TIMEOUT;
    }
    h2_g99_e2e_sleep(e2e, 1u);
  }
}

/* Waits until Main read the lifted finger again H2_G99_E2E_LIFT_MS after
 * it read the lift, which ends the press in Main. */
static h2_pal_result_t wait_lifted(h2_g99_e2e_t* e2e) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  h2_g99_e2e_lock(e2e);
  const uint64_t lifted_ms = f->last_read_ms;
  h2_g99_e2e_unlock(e2e);
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_e2e_lock(e2e);
    const bool held = f->last_read_ms >= lifted_ms + H2_G99_E2E_LIFT_MS;
    h2_g99_e2e_unlock(e2e);
    if (held) return H2_PAL_OK;
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (elapsed >= H2_G99_E2E_LIFT_MS + H2_G99_E2E_TOUCH_READ_MS ||
        !h2_g99_e2e_main_running(e2e) || h2_g99_e2e_aborted(e2e)) {
      h2_g99_e2e_fail(e2e, "Main did not read the lifted finger within %u ms",
                      (unsigned)elapsed);
      return H2_PAL_ERR_TIMEOUT;
    }
    h2_g99_e2e_sleep(e2e, 1u);
  }
}

/* Hands the touch source back to Runtime touch. The gap still open at the
 * end counts as well, so a Main that stopped reading shows. */
static void finish(h2_g99_e2e_t* e2e, h2_g99_e2e_touch_stats_t* out) {
  h2_g99_e2e_finger_t* f = &e2e->finger;
  const uint64_t now = h2_g99_e2e_now_ms(e2e);
  h2_g99_e2e_lock(e2e);
  const uint64_t since = f->reads > 0u ? f->last_read_ms : f->begun_ms;
  if (now > since && now - since > f->max_gap_ms)
    f->max_gap_ms = (uint32_t)(now - since);
  if (out != NULL) {
    out->reads = f->reads;
    out->max_gap_ms = f->max_gap_ms;
  }
  f->scripted = false;
  f->down = false;
  f->hovering = false;
  h2_g99_e2e_unlock(e2e);
}

/* Lifts the finger and waits until Main read it and ended the press, then
 * hands touch back. */
static h2_pal_result_t lift(h2_g99_e2e_t* e2e, h2_g99_e2e_touch_stats_t* out) {
  place(e2e, false, 0, 0);
  h2_pal_result_t rc = wait_read(e2e, "lift");
  if (rc == H2_PAL_OK) rc = wait_lifted(e2e);
  finish(e2e, out);
  return rc;
}

h2_pal_result_t h2_g99_e2e_drag(h2_g99_e2e_t* e2e, int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1, uint32_t duration_ms,
                                bool sampled, h2_g99_e2e_touch_stats_t* out) {
  begin(e2e);
  place(e2e, true, x0, y0);
  h2_pal_result_t rc = sampled ? wait_read(e2e, "down") : H2_PAL_OK;
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  while (rc == H2_PAL_OK) {
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (elapsed >= duration_ms) break;
    const int64_t t = (int64_t)elapsed;
    const int64_t d = (int64_t)duration_ms;
    place(e2e, true, x0 + (int32_t)((int64_t)(x1 - x0) * t / d),
          y0 + (int32_t)((int64_t)(y1 - y0) * t / d));
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_TOUCH_MOVE_MS);
  }
  if (rc == H2_PAL_OK) {
    place(e2e, true, x1, y1);
    if (sampled) rc = wait_read(e2e, "end point");
  }
  const h2_pal_result_t lifted = lift(e2e, out);
  return rc != H2_PAL_OK ? rc : lifted;
}

h2_pal_result_t h2_g99_e2e_tap(h2_g99_e2e_t* e2e, int32_t x, int32_t y) {
  begin(e2e);
  place(e2e, true, x, y);
  h2_pal_result_t rc = wait_read(e2e, "tap");
  if (rc == H2_PAL_OK) h2_g99_e2e_sleep(e2e, H2_G99_E2E_KEY_HOLD_MS);
  const h2_pal_result_t lifted = lift(e2e, NULL);
  return rc != H2_PAL_OK ? rc : lifted;
}

h2_pal_result_t h2_g99_e2e_touch_down(h2_g99_e2e_t* e2e, int32_t x, int32_t y) {
  begin(e2e);
  place(e2e, true, x, y);
  const h2_pal_result_t rc = wait_read(e2e, "down");
  if (rc != H2_PAL_OK) (void)lift(e2e, NULL);
  return rc;
}

h2_pal_result_t h2_g99_e2e_touch_move(h2_g99_e2e_t* e2e, int32_t x, int32_t y) {
  place(e2e, true, x, y);
  return wait_read(e2e, "move");
}

h2_pal_result_t h2_g99_e2e_touch_up(h2_g99_e2e_t* e2e) {
  return lift(e2e, NULL);
}

h2_pal_result_t h2_g99_e2e_touch_probe(h2_g99_e2e_t* e2e, uint32_t duration_ms,
                                       h2_g99_e2e_touch_stats_t* out) {
  begin(e2e);
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  while (h2_g99_e2e_now_ms(e2e) - start < duration_ms) {
    if (!h2_g99_e2e_main_running(e2e) || h2_g99_e2e_aborted(e2e)) break;
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
  finish(e2e, out);
  return h2_g99_e2e_main_running(e2e) ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_g99_e2e_hover(h2_g99_e2e_t* e2e, int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1, uint32_t duration_ms) {
  begin(e2e);
  place_hover(e2e, x0, y0);
  h2_pal_result_t rc = wait_read(e2e, "hover");
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  while (rc == H2_PAL_OK) {
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (elapsed >= duration_ms) break;
    const int64_t t = (int64_t)elapsed;
    const int64_t d = (int64_t)duration_ms;
    place_hover(e2e, x0 + (int32_t)((int64_t)(x1 - x0) * t / d),
                y0 + (int32_t)((int64_t)(y1 - y0) * t / d));
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_TOUCH_MOVE_MS);
  }
  if (rc == H2_PAL_OK) {
    place_hover(e2e, x1, y1);
    rc = wait_read(e2e, "hover end point");
  }
  finish(e2e, NULL);
  return rc;
}

void h2_g99_e2e_touch_model(h2_g99_e2e_t* e2e, h2_g99_e2e_touch_model_t model) {
  h2_g99_e2e_lock(e2e);
  e2e->finger.model = model;
  e2e->finger.report_ms = 0u;
  h2_g99_e2e_unlock(e2e);
}
