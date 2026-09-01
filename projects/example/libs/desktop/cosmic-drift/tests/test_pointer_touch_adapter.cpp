#include "pointer_touch_adapter.h"

#include <cassert>

int main() {
  h2::cosmic_drift::PointerTouchAdapter invalid_adapter(0u, 448u);
  assert(h2_pal_touch_open(invalid_adapter.api()) == H2_PAL_ERR_INVALID_ARG);

  h2::cosmic_drift::PointerTouchAdapter adapter(368u, 448u);
  const h2_pal_touch_api_t *api = adapter.api();
  h2_pal_touch_event_t event = {};
  h2_pal_touch_info_t info = {};
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_open(api) == H2_PAL_OK);
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_OK);
  assert(info.width == 368u && info.height == 448u);

  adapter.update(100, 200, false);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 100 && event.y == 200);
  adapter.update(100, 200, false);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);

  adapter.update(101, 201, false);
  adapter.update(102, 202, false);
  adapter.update(103, 203, false);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 103 && event.y == 203);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);

  adapter.update(120, 220, true);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 120 && event.y == 220);

  adapter.update(120, 220, false);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP);

  adapter.update(-10, 900, false);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 0 && event.y == 447);

  for (int index = 0; index < 20; ++index) {
    adapter.update(index, index + 10, index % 2 == 0);
  }
  for (int index = 4; index < 20; ++index) {
    assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
    assert(event.kind ==
           (index % 2 == 0 ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP));
    assert(event.x == index && event.y == index + 10);
  }
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);

  assert(h2_pal_touch_close(api) == H2_PAL_OK);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_close(api) == H2_PAL_OK);
  return 0;
}
