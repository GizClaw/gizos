#include "h2_g99_touch.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* ---------- scripted source and clock ---------- */

typedef struct step {
  h2_pal_result_t rc;
  h2_pal_touch_event_kind_t kind;
  int32_t x;
  int32_t y;
} step_t;

static step_t g_steps[32];
static size_t g_count;
static size_t g_next;
static uint32_t g_calls;
static uint64_t g_now;

static h2_pal_result_t source_open(void* user) {
  (void)user;
  return H2_PAL_OK;
}

static h2_pal_result_t source_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  *out = (h2_pal_touch_info_t){480u, 480u};
  return H2_PAL_OK;
}

/* Answers the queued steps, then has nothing new. */
static h2_pal_result_t source_poll(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  ++g_calls;
  if (g_next >= g_count) return H2_PAL_ERR_WOULD_BLOCK;
  const step_t* s = &g_steps[g_next++];
  if (s->rc == H2_PAL_OK) *out = (h2_pal_touch_event_t){s->kind, s->x, s->y};
  return s->rc;
}

static h2_pal_result_t source_close(void* user) {
  (void)user;
  return H2_PAL_OK;
}

static const h2_pal_touch_vtable_t kSource = {source_open, source_info,
                                              source_poll, source_close};
static const h2_pal_touch_api_t kSourceApi = {NULL, &kSource};

static h2_pal_result_t clock_ms(void* user, uint64_t* out) {
  (void)user;
  *out = g_now;
  return H2_PAL_OK;
}

static const h2_pal_time_vtable_t kClock = {.get_monotonic_ms = clock_ms};
static const h2_pal_time_api_t kClockApi = {NULL, &kClock};

static h2_g99_touch_filter_t g_filter;

static const h2_pal_touch_api_t* setup(const h2_pal_time_api_t* time) {
  g_count = g_next = 0u;
  g_calls = 0u;
  g_now = 1000u;
  h2_g99_touch_filter_init(&g_filter, &kSourceApi, time);
  const h2_pal_touch_api_t* api = h2_g99_touch_filter_api(&g_filter);
  CHECK(h2_pal_touch_open(api) == H2_PAL_OK);
  return api;
}

static void push(h2_pal_result_t rc, h2_pal_touch_event_kind_t kind, int32_t x,
                 int32_t y) {
  if (g_count < sizeof(g_steps) / sizeof(g_steps[0]))
    g_steps[g_count++] = (step_t){rc, kind, x, y};
}

static void contact(h2_pal_touch_event_kind_t kind, int32_t x, int32_t y) {
  push(H2_PAL_OK, kind, x, y);
}

/* Reads the filter at time t. */
static h2_pal_result_t poll_at(const h2_pal_touch_api_t* api, uint64_t t,
                               h2_pal_touch_event_t* out) {
  g_now = t;
  memset(out, 0, sizeof(*out));
  return h2_pal_touch_poll_event(api, out);
}

static bool is(h2_pal_result_t rc, const h2_pal_touch_event_t* e,
               h2_pal_touch_event_kind_t kind, int32_t x, int32_t y) {
  return rc == H2_PAL_OK && e->kind == kind && e->x == x && e->y == y;
}

/* ---------- cases ---------- */

/* A clean source passes through; the release counts once it held. */
static void test_clean_press(void) {
  const h2_pal_touch_api_t* api = setup(&kClockApi);
  h2_pal_touch_event_t e;
  h2_pal_touch_info_t info;
  CHECK(h2_pal_touch_get_info(api, &info) == H2_PAL_OK && info.width == 480u);
  contact(H2_PAL_TOUCH_EVENT_DOWN, 10, 20);
  CHECK(is(poll_at(api, 1000u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 10, 20));
  contact(H2_PAL_TOUCH_EVENT_MOVE, 10, 30);
  CHECK(is(poll_at(api, 1012u, &e), &e, H2_PAL_TOUCH_EVENT_MOVE, 10, 30));
  contact(H2_PAL_TOUCH_EVENT_UP, 10, 30);
  CHECK(poll_at(api, 1030u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(poll_at(api, 1060u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(is(poll_at(api, 1080u, &e), &e, H2_PAL_TOUCH_EVENT_UP, 10, 30));
  CHECK(poll_at(api, 1100u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(g_filter.stats.rejoined == 0u && g_filter.stats.failed == 0u);
}

/* LVGL reads again right after each event: one source read per burst. */
static void test_read_interval(void) {
  const h2_pal_touch_api_t* api = setup(&kClockApi);
  h2_pal_touch_event_t e;
  contact(H2_PAL_TOUCH_EVENT_DOWN, 1, 1);
  CHECK(is(poll_at(api, 1000u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 1, 1));
  const uint32_t calls = g_calls;
  CHECK(poll_at(api, 1000u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(poll_at(api, 1009u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(g_calls == calls);
  CHECK(poll_at(api, 1010u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(g_calls == calls + 1u);
}

/* Mosaico CST9220, driver 0.1.1: a read between two reports finds an empty
 * frame, which the board reports as a release; the next report presses
 * again. The press continues, also when the next read comes late. */
static void test_empty_frames(void) {
  const h2_pal_touch_api_t* api = setup(&kClockApi);
  h2_pal_touch_event_t e;
  contact(H2_PAL_TOUCH_EVENT_DOWN, 50, 50);
  CHECK(is(poll_at(api, 1000u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 50, 50));
  contact(H2_PAL_TOUCH_EVENT_UP, 50, 50);
  CHECK(poll_at(api, 1016u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  contact(H2_PAL_TOUCH_EVENT_DOWN, 50, 60);
  CHECK(is(poll_at(api, 1032u, &e), &e, H2_PAL_TOUCH_EVENT_MOVE, 50, 60));
  contact(H2_PAL_TOUCH_EVENT_UP, 50, 60);
  CHECK(poll_at(api, 1048u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  /* A 150 ms frame: the finger is still there at the next read. */
  contact(H2_PAL_TOUCH_EVENT_DOWN, 50, 90);
  CHECK(is(poll_at(api, 1200u, &e), &e, H2_PAL_TOUCH_EVENT_MOVE, 50, 90));
  CHECK(g_filter.stats.rejoined == 2u);
  /* The finger lifts: the last release holds. */
  contact(H2_PAL_TOUCH_EVENT_UP, 50, 90);
  CHECK(poll_at(api, 1216u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(poll_at(api, 1240u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(is(poll_at(api, 1270u, &e), &e, H2_PAL_TOUCH_EVENT_UP, 50, 90));
}

/* Mosaico CST9220, driver 0.1.2: a read between two reports fails. A press
 * whose source keeps failing ends where it was, and the finger presses
 * again once the source answers. */
static void test_failed_reads(void) {
  const h2_pal_touch_api_t* api = setup(&kClockApi);
  h2_pal_touch_event_t e;
  contact(H2_PAL_TOUCH_EVENT_DOWN, 5, 5);
  CHECK(is(poll_at(api, 1000u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 5, 5));
  push(H2_PAL_ERR_IO, H2_PAL_TOUCH_EVENT_UP, 0, 0);
  CHECK(poll_at(api, 1016u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(poll_at(api, 1032u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  push(H2_PAL_ERR_IO, H2_PAL_TOUCH_EVENT_UP, 0, 0);
  CHECK(poll_at(api, 1048u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  contact(H2_PAL_TOUCH_EVENT_MOVE, 5, 9);
  CHECK(is(poll_at(api, 1064u, &e), &e, H2_PAL_TOUCH_EVENT_MOVE, 5, 9));
  CHECK(g_filter.stats.failed == 2u);
  /* The source stops answering. */
  uint64_t t = 1080u;
  for (; t < 1080u + H2_G99_TOUCH_LOST_MS; t += 20u) {
    push(H2_PAL_ERR_IO, H2_PAL_TOUCH_EVENT_UP, 0, 0);
    CHECK(poll_at(api, t, &e) == H2_PAL_ERR_WOULD_BLOCK);
  }
  push(H2_PAL_ERR_IO, H2_PAL_TOUCH_EVENT_UP, 0, 0);
  CHECK(is(poll_at(api, t, &e), &e, H2_PAL_TOUCH_EVENT_UP, 5, 9));
  /* It answers again with the finger moved: a new press. */
  contact(H2_PAL_TOUCH_EVENT_MOVE, 5, 12);
  CHECK(is(poll_at(api, t + 20u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 5, 12));
}

/* Web: a mouse moving without a pressed button is no contact, also right
 * after a release. */
static void test_hover(void) {
  const h2_pal_touch_api_t* api = setup(&kClockApi);
  h2_pal_touch_event_t e;
  contact(H2_PAL_TOUCH_EVENT_MOVE, 1, 1);
  CHECK(poll_at(api, 1000u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  contact(H2_PAL_TOUCH_EVENT_MOVE, 1, 2);
  contact(H2_PAL_TOUCH_EVENT_MOVE, 1, 3);
  CHECK(poll_at(api, 1020u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(g_filter.stats.hovers == 3u);
  contact(H2_PAL_TOUCH_EVENT_DOWN, 1, 3);
  CHECK(is(poll_at(api, 1040u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 1, 3));
  contact(H2_PAL_TOUCH_EVENT_MOVE, 1, 9);
  CHECK(is(poll_at(api, 1060u, &e), &e, H2_PAL_TOUCH_EVENT_MOVE, 1, 9));
  contact(H2_PAL_TOUCH_EVENT_UP, 1, 9);
  contact(H2_PAL_TOUCH_EVENT_MOVE, 1, 12);
  CHECK(poll_at(api, 1080u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(is(poll_at(api, 1130u, &e), &e, H2_PAL_TOUCH_EVENT_UP, 1, 9));
  CHECK(g_filter.stats.rejoined == 0u);
}

/* Without a clock every read reaches the source and a release counts at
 * the next read without contact. */
static void test_without_clock(void) {
  const h2_pal_touch_api_t* api = setup(NULL);
  h2_pal_touch_event_t e;
  contact(H2_PAL_TOUCH_EVENT_DOWN, 3, 4);
  CHECK(is(poll_at(api, 0u, &e), &e, H2_PAL_TOUCH_EVENT_DOWN, 3, 4));
  contact(H2_PAL_TOUCH_EVENT_UP, 3, 4);
  CHECK(is(poll_at(api, 0u, &e), &e, H2_PAL_TOUCH_EVENT_UP, 3, 4));
  CHECK(poll_at(api, 0u, &e) == H2_PAL_ERR_WOULD_BLOCK);
  CHECK(h2_pal_touch_close(api) == H2_PAL_OK);
}

int main(void) {
  test_clean_press();
  test_read_interval();
  test_empty_frames();
  test_failed_reads();
  test_hover();
  test_without_clock();
  if (failures != 0) {
    fprintf(stderr, "%d touch check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
