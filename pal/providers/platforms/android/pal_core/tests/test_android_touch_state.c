#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_android_touch_state.h"
#include <assert.h>

int main(void) {
  h2_android_touch_state_t state = {0};
  h2_pal_touch_event_t event;
  assert(h2_android_touch_snapshot(&state, 480, 800, 0, 0, false, &event) ==
         H2_PAL_ERR_INVALID_STATE);
  state.opened = true;
  assert(h2_android_touch_snapshot(&state, 480, 800, 0, 0, false, &event) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_android_touch_snapshot(&state, 480, 800, 10, 20, true, &event) ==
         H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN && event.x == 10 &&
         event.y == 20);
  assert(h2_android_touch_snapshot(&state, 480, 800, 10, 20, true, &event) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_android_touch_snapshot(&state, 480, 800, 479, 799, true, &event) ==
         H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE && event.x == 479 &&
         event.y == 799);
  assert(h2_android_touch_snapshot(&state, 480, 800, 480, 800, true, &event) ==
         H2_PAL_ERR_IO);
  assert(h2_android_touch_snapshot(&state, 480, 800, -1, -1, false, &event) ==
         H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP && event.x == 479 &&
         event.y == 799);
  assert(h2_android_touch_snapshot(&state, 480, 800, 0, 0, false, &event) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_android_touch_snapshot(&state, 480, 800, 10, 20, true, &event) ==
         H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(h2_android_touch_snapshot(&state, 0, 800, 0, 0, true, &event) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_android_touch_snapshot(&state, 480, 800, 0, 0, true, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  return 0;
}
