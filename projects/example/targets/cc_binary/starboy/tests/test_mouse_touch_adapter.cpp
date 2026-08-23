#include "mouse_touch_adapter.h"

#include <cassert>

int main() {
  h2::starboy::MouseTouchAdapter adapter(368u, 448u);
  const h2_pal_touch_api_t *api = adapter.api();
  assert(h2_pal_touch_open(api) == H2_PAL_OK);

  h2_pal_touch_info_t info = {};
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_OK);
  assert(info.width == 368u && info.height == 448u);

  // No mouse button state is supplied; hover still begins a Touch lifecycle.
  adapter.update(true, -20, 900);
  h2_pal_touch_event_t event = {};
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 0 && event.y == 447);

  adapter.update(true, 100, 200);
  adapter.update(true, 101, 201);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 101 && event.y == 201);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);

  for (std::int32_t coordinate = 102; coordinate < 300; ++coordinate) {
    adapter.update(true, coordinate, coordinate + 20);
  }
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 299 && event.y == 319);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);

  adapter.update_global_coordinates(520, 620, 400, 350);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 120 && event.y == 270);

  adapter.update_global_coordinates(50, 900, 400, 350);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 0 && event.y == 447);

  adapter.update(false, 101, 201);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP);
  assert(h2_pal_touch_close(api) == H2_PAL_OK);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
