#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_android_touch_state.h"
#include <assert.h>
#include <stddef.h>

static void capture(h2_android_touch_state_t* state, int x, int y, bool down) {
  assert(h2_android_touch_capture(state, 480, 800, x, y, down) == H2_PAL_OK);
}
static void expect(h2_android_touch_state_t* state,
                   h2_pal_touch_event_kind_t kind, int x, int y) {
  h2_pal_touch_event_t event;
  assert(h2_android_touch_poll(state, &event) == H2_PAL_OK);
  assert(event.kind == kind && event.x == x && event.y == y);
}
static void expect_empty(h2_android_touch_state_t* state) {
  h2_pal_touch_event_t event;
  assert(h2_android_touch_poll(state, &event) == H2_PAL_ERR_WOULD_BLOCK);
}
int main(void) {
  h2_android_touch_state_t state = {0};
  h2_pal_touch_event_t event;
  assert(h2_android_touch_poll(&state, &event) == H2_PAL_ERR_INVALID_STATE);
  state.opened = true;
  expect_empty(&state);

  /* Preserve a release and a new press delivered entirely between polls. */
  capture(&state, 10, 20, true);
  capture(&state, 30, 40, true);
  capture(&state, 40, 50, true);
  capture(&state, 0, 0, false);
  capture(&state, 100, 200, true);
  expect(&state, H2_PAL_TOUCH_EVENT_DOWN, 10, 20);
  expect(&state, H2_PAL_TOUCH_EVENT_MOVE, 40, 50);
  expect(&state, H2_PAL_TOUCH_EVENT_UP, 40, 50);
  expect(&state, H2_PAL_TOUCH_EVENT_DOWN, 100, 200);
  expect_empty(&state);

  /* A complete tap must not disappear when both edges precede the poll. */
  capture(&state, 0, 0, false);
  capture(&state, 200, 300, true);
  capture(&state, 0, 0, false);
  expect(&state, H2_PAL_TOUCH_EVENT_UP, 100, 200);
  expect(&state, H2_PAL_TOUCH_EVENT_DOWN, 200, 300);
  expect(&state, H2_PAL_TOUCH_EVENT_UP, 200, 300);
  expect_empty(&state);

  capture(&state, 479, 799, true);
  assert(h2_android_touch_capture(&state, 480, 800, 480, 800, true) ==
         H2_PAL_ERR_IO);
  assert(h2_android_touch_poll(&state, &event) == H2_PAL_ERR_IO);
  capture(&state, 0, 0, true);
  expect(&state, H2_PAL_TOUCH_EVENT_DOWN, 0, 0);
  capture(&state, -1, -1, false);
  expect(&state, H2_PAL_TOUCH_EVENT_UP, 0, 0);

  /* If edges alone fill the finite queue, explicit loss precedes a fresh
   * DOWN; it never appears as a MOVE joining unrelated strokes. */
  state = (h2_android_touch_state_t){.opened = true};
  for (unsigned i = 0; i < H2_ANDROID_TOUCH_QUEUE_CAPACITY / 2; ++i) {
    capture(&state, 10, 20, true);
    capture(&state, 0, 0, false);
  }
  capture(&state, 123, 456, true);
  assert(h2_android_touch_poll(&state, &event) == H2_PAL_ERR_IO);
  expect(&state, H2_PAL_TOUCH_EVENT_DOWN, 123, 456);
  expect_empty(&state);

  /* Under pressure discard movement rather than a stroke boundary. */
  state = (h2_android_touch_state_t){.opened = true};
  for (int i = 0; i < 21; ++i) {
    capture(&state, i, 20, true);
    capture(&state, i + 1, 30, true);
    capture(&state, 0, 0, false);
  }
  capture(&state, 300, 400, true);
  capture(&state, 0, 0, false);
  unsigned downs = 0, ups = 0;
  bool pressed = false;
  while (h2_android_touch_poll(&state, &event) == H2_PAL_OK) {
    if (event.kind == H2_PAL_TOUCH_EVENT_DOWN) {
      assert(!pressed);
      pressed = true;
      ++downs;
    } else if (event.kind == H2_PAL_TOUCH_EVENT_UP) {
      assert(pressed);
      pressed = false;
      ++ups;
    } else
      assert(pressed);
  }
  assert(downs == 22 && ups == 22 && !pressed);
  expect_empty(&state);
  assert(h2_android_touch_capture(&state, 0, 800, 0, 0, true) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_android_touch_poll(&state, NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
