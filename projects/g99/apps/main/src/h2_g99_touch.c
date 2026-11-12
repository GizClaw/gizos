#include "h2_g99_touch.h"

#include <string.h>

/* Events read from the source in one read: releases and hovering moves are
 * consumed without reporting, a contact is reported at once. */
#define H2_G99_TOUCH_BURST 16

static bool clock_ms(const h2_g99_touch_filter_t* f, uint64_t* out) {
  *out = 0u;
  return f->time != NULL &&
         h2_pal_time_get_monotonic_ms(f->time, out) == H2_PAL_OK;
}

static void reset(h2_g99_touch_filter_t* f) {
  f->source_down = false;
  f->down = false;
  f->releasing = false;
  f->failing = false;
  f->read_once = false;
  f->pending_down = false;
}

static h2_pal_result_t release(h2_g99_touch_filter_t* f,
                               h2_pal_touch_event_t* out) {
  f->down = false;
  f->releasing = false;
  f->failing = false;
  *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, f->x, f->y};
  return H2_PAL_OK;
}

/* Without a new contact: reports a release that held, or the end of a press
 * whose source kept failing. */
static h2_pal_result_t settle(h2_g99_touch_filter_t* f, bool timed,
                              uint64_t now, h2_pal_touch_event_t* out) {
  if (!f->down) return H2_PAL_ERR_WOULD_BLOCK;
  const bool released = f->releasing && (!timed || now - f->release_ms >=
                                                       H2_G99_TOUCH_RELEASE_MS);
  const bool lost =
      f->failing && (!timed || now - f->fail_ms >= H2_G99_TOUCH_LOST_MS);
  if (!released && !lost) return H2_PAL_ERR_WOULD_BLOCK;
  return release(f, out);
}

static h2_pal_result_t filter_poll(void* user, h2_pal_touch_event_t* out) {
  h2_g99_touch_filter_t* f = user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  uint64_t now = 0u;
  const bool timed = clock_ms(f, &now);
  if (f->pending_down) {
    f->pending_down = false;
    f->source_down = true;
    f->down = true;
    f->x = f->pending_contact.x;
    f->y = f->pending_contact.y;
    *out = f->pending_contact;
    return H2_PAL_OK;
  }
  if (f->releasing &&
      (!timed || now - f->release_ms >= H2_G99_TOUCH_RELEASE_MS))
    return release(f, out);
  /* LVGL reads again right after each event; one source read per burst
   * keeps those reads off a controller that has no new report yet. */
  if (timed && f->read_once && now - f->read_ms < H2_G99_TOUCH_READ_MS)
    return settle(f, timed, now, out);
  f->read_once = true;
  f->read_ms = now;
  for (int i = 0; i < H2_G99_TOUCH_BURST; ++i) {
    h2_pal_touch_event_t event;
    memset(&event, 0, sizeof(event));
    const h2_pal_result_t rc = h2_pal_touch_poll_event(f->source, &event);
    ++f->stats.reads;
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      f->failing = false;
      break;
    }
    if (rc != H2_PAL_OK) {
      /* No report this time, or a failed bus read: no news either way. */
      ++f->stats.failed;
      if (f->down && !f->failing) {
        f->failing = true;
        f->fail_ms = now;
      }
      break;
    }
    f->failing = false;
    if (event.kind == H2_PAL_TOUCH_EVENT_UP) {
      f->source_down = false;
      if (f->down && !f->releasing) {
        f->releasing = true;
        f->release_ms = now;
        f->x = event.x;
        f->y = event.y;
      }
      continue;
    }
    if (event.kind == H2_PAL_TOUCH_EVENT_MOVE && !f->source_down) {
      ++f->stats.hovers;
      continue;
    }
    if (event.kind != H2_PAL_TOUCH_EVENT_DOWN &&
        event.kind != H2_PAL_TOUCH_EVENT_MOVE)
      continue;
    if (event.kind == H2_PAL_TOUCH_EVENT_DOWN && f->down && f->source_down &&
        !f->releasing) {
      f->pending_down = true;
      f->pending_contact = event;
      return release(f, out);
    }
    f->source_down = true;
    f->x = event.x;
    f->y = event.y;
    if (f->releasing) {
      /* The contact came back before its release held: the same press. */
      f->releasing = false;
      ++f->stats.rejoined;
    }
    *out = (h2_pal_touch_event_t){
        f->down ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN, event.x,
        event.y};
    f->down = true;
    return H2_PAL_OK;
  }
  return settle(f, timed, now, out);
}

static h2_pal_result_t filter_open(void* user) {
  h2_g99_touch_filter_t* f = user;
  reset(f);
  return h2_pal_touch_open(f->source);
}

static h2_pal_result_t filter_get_info(void* user, h2_pal_touch_info_t* out) {
  h2_g99_touch_filter_t* f = user;
  return h2_pal_touch_get_info(f->source, out);
}

static h2_pal_result_t filter_close(void* user) {
  h2_g99_touch_filter_t* f = user;
  reset(f);
  return h2_pal_touch_close(f->source);
}

static const h2_pal_touch_vtable_t kFilterVtable = {
    filter_open, filter_get_info, filter_poll, filter_close};

void h2_g99_touch_filter_init(h2_g99_touch_filter_t* filter,
                              const h2_pal_touch_api_t* source,
                              const h2_pal_time_api_t* time) {
  if (filter == NULL) return;
  memset(filter, 0, sizeof(*filter));
  filter->source = source;
  filter->time = time;
  filter->api.user = filter;
  filter->api.vtable = &kFilterVtable;
}

const h2_pal_touch_api_t* h2_g99_touch_filter_api(
    h2_g99_touch_filter_t* filter) {
  return filter != NULL ? &filter->api : NULL;
}
