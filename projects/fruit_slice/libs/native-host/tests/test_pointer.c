#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_fruit_slice_native_host.h"
#include <assert.h>

typedef struct snapshot {
  int32_t x, y;
  int down, error;
} snapshot_t;
static h2_pal_result_t read_pointer(void* user, int32_t* x, int32_t* y,
                                    int* down) {
  snapshot_t* snapshot = user;
  *x = snapshot->x;
  *y = snapshot->y;
  *down = snapshot->down;
  return snapshot->error;
}
int main(void) {
  h2_fruit_slice_pointer_t pointer;
  snapshot_t snapshot = {10, 20, 0, 0};
  assert(h2_fruit_slice_pointer_init(&pointer, 480, 800, read_pointer,
                                     &snapshot) == 0);
  const h2_pal_touch_api_t* touch = h2_fruit_slice_pointer_touch(&pointer);
  h2_pal_touch_event_t event;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_open(touch) == 0);
  h2_pal_touch_info_t info;
  assert(h2_pal_touch_get_info(touch, &info) == 0 && info.width == 480 &&
         info.height == 800);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  snapshot.down = 1;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN && event.x == 10 &&
         event.y == 20);
  assert(h2_pal_touch_open(touch) ==
         0); /* Idempotent open retains the stroke. */
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  snapshot.error = H2_PAL_ERR_WOULD_BLOCK;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  snapshot.error = 0;
  snapshot.x = 30;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE && event.x == 30);
  snapshot.x = 480;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  snapshot.down = 0;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP && event.x == 30 && event.y == 20);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_touch_close(touch) == 0);
  assert(h2_pal_touch_open(touch) == 0);
  snapshot.down = 1;
  snapshot.x = 40;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(h2_pal_touch_close(touch) == 0);
  assert(h2_fruit_slice_pointer_init(&pointer, 0, 800, read_pointer,
                                     &snapshot) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
