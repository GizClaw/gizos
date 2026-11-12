#include "h2_g99_e2e_internal.h"

#include <string.h>

/* Touch cases drag and tap with the scripted finger across the wheel and
 * information areas, using the published screen dimensions and layout. */

static h2_pal_result_t menu_at(h2_g99_e2e_t* e2e, int selection,
                               const char* what, h2_g99_observation_t* out) {
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(selection);
  return h2_g99_e2e_wait(e2e, &menu, NULL, NULL, H2_G99_E2E_NAV_MS, what, out);
}

/* A point on the wheel column, `permille` of the screen height below the
 * centre (negative is above). */
static void wheel_point(const h2_g99_observation_t* o, int32_t permille,
                        int32_t* out_x, int32_t* out_y) {
  int32_t x = (o->wheel_rect.x1 + o->wheel_rect.x2) / 2;
  if (x >= o->info_rect.x1) x = o->info_rect.x1 - 1;
  *out_x = x;
  *out_y = o->height / 2 + o->height * permille / 1000;
}

static void report_frames(h2_g99_e2e_t* e2e, const char* phase,
                          const h2_g99_e2e_frames_t* f) {
  const uint32_t intervals = f->frames > 1u ? f->frames - 1u : 0u;
  h2_g99_e2e_metric(
      e2e,
      "phase=%s frames=%u interval_avg_us=%u interval_max_us=%u "
      "work_max_us=%u",
      phase, (unsigned)f->frames,
      intervals > 0u ? (unsigned)(f->interval_us / intervals) : 0u,
      (unsigned)f->max_interval_us, (unsigned)f->max_work_us);
}

/* From the centre the finger drags a quarter screen up, which brings the
 * next item (Lab) to the centre, then a quarter screen down, which brings
 * Library back. A drag that Main read as a tap at its start would enter
 * Library instead, and on Lab would do nothing. */
static h2_pal_result_t drag_there_and_back(h2_g99_e2e_t* e2e,
                                           uint32_t duration_ms, bool sampled,
                                           int32_t x_permille) {
  h2_g99_observation_t o;
  h2_pal_result_t rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &o);
  if (rc != H2_PAL_OK) return rc;
  int32_t x = 0, centre = 0, above = 0, below = 0;
  wheel_point(&o, 0, &x, &centre);
  wheel_point(&o, -250, &x, &above);
  wheel_point(&o, 250, &x, &below);
  if (x_permille >= 0) x = o.width * x_permille / 1000;
  h2_g99_e2e_touch_stats_t up = {0u, 0u};
  h2_g99_e2e_touch_stats_t down = {0u, 0u};
  h2_g99_e2e_frames_t frames = {0};
  h2_g99_e2e_frames_reset(e2e);
  rc = h2_g99_e2e_drag(e2e, x, centre, x, above, duration_ms, sampled, &up);
  if (rc == H2_PAL_OK)
    rc = menu_at(e2e, H2_G99_MAIN_LAB, "MAIN(Lab) after dragging up", NULL);
  h2_g99_e2e_frames_get(e2e, &frames);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, x, centre, x, below, duration_ms, sampled, &down);
  if (rc == H2_PAL_OK)
    rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library) after dragging down",
                 NULL);
  h2_g99_e2e_metric(e2e,
                    "drag_ms=%u sampled=%d distance_px=%d up_reads=%u "
                    "up_max_gap_ms=%u down_reads=%u down_max_gap_ms=%u",
                    (unsigned)duration_ms, sampled ? 1 : 0,
                    (int)(centre - above), (unsigned)up.reads,
                    (unsigned)up.max_gap_ms, (unsigned)down.reads,
                    (unsigned)down.max_gap_ms);
  report_frames(e2e, "drag-up", &frames);
  return rc;
}

/* A deliberate drag: Main reads its start and end point. */
static h2_pal_result_t wheel_drag(h2_g99_e2e_t* e2e) {
  return drag_there_and_back(e2e, 400u, true, -1);
}

typedef struct drag_range {
  float min;
  float max;
} drag_range_t;

static bool drag_within(const h2_g99_observation_t* o, const void* arg) {
  const drag_range_t* range = arg;
  return o->wheel_drag >= range->min && o->wheel_drag <= range->max;
}

/* Main publishes a drag in the loop before the one that draws it, and draws
 * nothing else at rest: waits for that frame, up to the touch read budget. */
static void wait_frame(h2_g99_e2e_t* e2e, h2_g99_e2e_frames_t* out) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_e2e_frames_get(e2e, out);
    if (out->frames > 0u ||
        h2_g99_e2e_now_ms(e2e) - start >= H2_G99_E2E_TOUCH_READ_MS)
      return;
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

/* A held finger turns the wheel with it: a tenth of the screen up turns it
 * part of an item (0.43 on the shipped layouts) while the selection stays,
 * and Main draws frames meanwhile. Back at the start and lifted, the wheel
 * stays where it was. */
static h2_pal_result_t wheel_follow_at(h2_g99_e2e_t* e2e, int32_t x_permille) {
  h2_g99_observation_t o;
  h2_pal_result_t rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &o);
  if (rc != H2_PAL_OK) return rc;
  int32_t x = 0, centre = 0, up = 0;
  wheel_point(&o, 0, &x, &centre);
  wheel_point(&o, -100, &x, &up);
  if (x_permille >= 0) x = o.width * x_permille / 1000;
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  const drag_range_t turned = {0.2f, 0.7f};
  const drag_range_t back = {-0.05f, 0.05f};
  h2_g99_e2e_frames_t frames = {0};
  h2_g99_e2e_frames_reset(e2e);
  rc = h2_g99_e2e_touch_down(e2e, x, centre);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_g99_e2e_touch_move(e2e, x, up);
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_wait(e2e, &menu, drag_within, &turned, H2_G99_E2E_NAV_MS,
                        "the wheel turned part of an item with the finger", &o);
  wait_frame(e2e, &frames);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, x, centre);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &menu, drag_within, &back, H2_G99_E2E_NAV_MS,
                         "the wheel back with the finger", NULL);
  const h2_pal_result_t lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &menu, drag_within, &back, H2_G99_E2E_NAV_MS,
                         "MAIN(Library) after letting go", NULL);
  h2_g99_e2e_metric(e2e, "phase=follow distance_px=%d wheel_drag_pct=%d",
                    (int)(centre - up), (int)(o.wheel_drag * 100.0f));
  report_frames(e2e, "follow", &frames);
  if (rc == H2_PAL_OK && frames.frames == 0u) {
    h2_g99_e2e_fail(e2e, "no frame drawn while the finger turned the wheel");
    return H2_PAL_ERR_INVALID_STATE;
  }
  return rc;
}

static h2_pal_result_t wheel_follow(h2_g99_e2e_t* e2e) {
  return wheel_follow_at(e2e, -1);
}

static h2_pal_result_t wheel_restart(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t observation;
  h2_pal_result_t rc =
      menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &observation);
  if (rc != H2_PAL_OK) return rc;
  const int32_t horizontal = observation.width * 4 / 5;
  const int32_t first_start = observation.height / 2;
  const int32_t first_end = observation.height / 4;
  const int32_t second_start = observation.height * 9 / 10;
  const int32_t second_end = observation.height * 13 / 20;
  const drag_range_t forward = {0.5f, 1.001f};
  const drag_range_t settled = {-0.001f, 0.001f};
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  const h2_g99_e2e_expect_t lab = h2_g99_e2e_main_ready(H2_G99_MAIN_LAB);
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  rc = h2_g99_e2e_touch_down(e2e, horizontal, first_start);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, horizontal, first_end);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &library, drag_within, &forward,
                         H2_G99_E2E_NAV_MS, "first upward drag held", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_down(e2e, horizontal, second_start);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &lab, drag_within, &settled, H2_G99_E2E_NAV_MS,
                         "new contact settles the missing release", NULL);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, horizontal, second_end);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &lab, drag_within, &forward, H2_G99_E2E_NAV_MS,
                         "second upward drag uses its own origin", NULL);
  const h2_pal_result_t lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &settings, drag_within, &settled,
                         H2_G99_E2E_NAV_MS, "second upward drag snapped", NULL);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static h2_pal_result_t full_screen_drag(h2_g99_e2e_t* e2e) {
  const int32_t columns[] = {50, 500, 950};
  for (size_t index = 0u; index < sizeof(columns) / sizeof(columns[0]);
       ++index) {
    h2_pal_result_t rc = wheel_follow_at(e2e, columns[index]);
    if (rc == H2_PAL_OK)
      rc = drag_there_and_back(e2e, 400u, true, columns[index]);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

/* A quick 150 ms flick that Main reads only when it gets to it, as with a
 * real finger. */
static h2_pal_result_t wheel_flick(h2_g99_e2e_t* e2e) {
  return drag_there_and_back(e2e, 150u, false, -1);
}

static h2_pal_result_t wheel_tap(h2_g99_e2e_t* e2e) {
  const int32_t positions[][2] = {
      {200, 100}, {200, 500}, {200, 900}, {800, 100}, {800, 500}, {800, 900},
  };
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  for (size_t index = 0u; index < sizeof(positions) / sizeof(positions[0]);
       ++index) {
    h2_g99_observation_t observation;
    h2_pal_result_t rc =
        menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &observation);
    if (rc == H2_PAL_OK)
      rc = h2_g99_e2e_tap(e2e, observation.width * positions[index][0] / 1000,
                          observation.height * positions[index][1] / 1000);
    if (rc == H2_PAL_OK)
      rc = h2_g99_e2e_wait(e2e, &library, NULL, NULL, H2_G99_E2E_NAV_MS,
                           "Library after a full-screen tap", NULL);
    if (rc == H2_PAL_OK)
      rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &menu,
                                  H2_G99_E2E_NAV_MS, "MAIN(Library)");
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t page_tap(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_VOLUME);
  h2_g99_observation_t observation;
  const h2_g99_e2e_expect_t volume =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_VOLUME);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &volume, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "Volume ready", &observation);
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_tap(e2e, observation.width * 4 / 5, observation.height / 10);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_stays(e2e, &volume, 200u,
                          "slider background does not enter editing");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_tap(e2e, observation.width / 10, observation.height / 10);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_stays(e2e, &volume, 200u,
                          "background taps leave the slider ready");
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &settings,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &library,
                                H2_G99_E2E_NAV_MS, "Library ready");
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_tap(e2e, observation.width * 4 / 5, observation.height / 10);
  h2_g99_e2e_expect_t running = h2_g99_e2e_any();
  running.route = H2_G99_ROUTE_APP;
  running.lifecycle = H2_G99_LIFECYCLE_APP_RUNNING;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &running, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "Orbit after tapping Library background", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &library,
                                H2_G99_E2E_NAV_MS, "Library after Orbit");
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &menu,
                                H2_G99_E2E_NAV_MS, "MAIN(Library)");
  return rc;
}

/* Requires MAIN(selection) at rest with the wheel undragged for
 * duration_ms. */
static h2_pal_result_t stays_undragged(h2_g99_e2e_t* e2e, int selection,
                                       uint32_t duration_ms, const char* what) {
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(selection);
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_observation_t o;
    memset(&o, 0, sizeof(o));
    const bool observed = h2_g99_e2e_observe(e2e, &o, NULL);
    if (!observed || !h2_g99_e2e_matches(&o, &menu) || o.wheel_drag != 0.0f) {
      h2_g99_e2e_fail(e2e, "%s: wheel moved (drag %d%% main=%u transition=%d)",
                      what, (int)(o.wheel_drag * 100.0f),
                      (unsigned)o.main_selection, (int)o.transition);
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_g99_e2e_now_ms(e2e) - start >= duration_ms) return H2_PAL_OK;
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

/* A mouse moving over the wheel with no button pressed, as the Web platform
 * reports it, is no touch: the wheel stays. Pressed and dragged afterwards,
 * the same pointer turns the wheel one item and back. */
static h2_pal_result_t hover_ignored(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t o;
  h2_pal_result_t rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &o);
  if (rc != H2_PAL_OK) return rc;
  int32_t x = 0, centre = 0, above = 0;
  wheel_point(&o, 0, &x, &centre);
  wheel_point(&o, -250, &x, &above);
  rc = h2_g99_e2e_hover(e2e, x, centre, x, above, 300u);
  if (rc == H2_PAL_OK)
    rc = stays_undragged(e2e, H2_G99_MAIN_LIBRARY, 300u,
                         "after hovering over the wheel");
  if (rc == H2_PAL_OK) rc = drag_there_and_back(e2e, 400u, true, -1);
  return rc;
}

/* The Mosaico touch controller between two reports (see
 * h2_g99_e2e_touch_model_t): a held finger still turns the wheel with it,
 * and a deliberate drag still turns it one item and back. */
static h2_pal_result_t controller_drag(h2_g99_e2e_t* e2e,
                                       h2_g99_e2e_touch_model_t model) {
  h2_g99_e2e_touch_model(e2e, model);
  h2_pal_result_t rc = wheel_follow(e2e);
  if (rc == H2_PAL_OK) rc = drag_there_and_back(e2e, 400u, true, -1);
  return rc;
}

static h2_pal_result_t empty_frames(h2_g99_e2e_t* e2e) {
  return controller_drag(e2e, H2_G99_E2E_TOUCH_EMPTY_FRAMES);
}

static h2_pal_result_t failed_reads(h2_g99_e2e_t* e2e) {
  return controller_drag(e2e, H2_G99_E2E_TOUCH_FAILED_READS);
}

/* A full-height gesture requests several items, but the visible wheel
 * follows at bounded speed, reaches only one detent and always settles. */
static h2_pal_result_t wheel_limits(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t o;
  h2_pal_result_t rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "MAIN(Library)", &o);
  if (rc != H2_PAL_OK) return rc;
  int32_t x = 0, centre = 0;
  wheel_point(&o, 0, &x, &centre);
  const int32_t low = o.height * 9 / 10;
  const int32_t high = o.height / 10;
  const drag_range_t forward = {0.9f, 1.001f};
  const drag_range_t backward = {-1.001f, -0.9f};
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  const h2_g99_e2e_expect_t lab = h2_g99_e2e_main_ready(H2_G99_MAIN_LAB);
  rc = h2_g99_e2e_touch_down(e2e, x, low);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, x, high);
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_wait(e2e, &library, drag_within, &forward, H2_G99_E2E_NAV_MS,
                        "full drag bounded to one item", NULL);
  const h2_pal_result_t released = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = released;
  if (rc == H2_PAL_OK)
    rc = menu_at(e2e, H2_G99_MAIN_LAB, "Lab after bounded snap", &o);
  if (rc == H2_PAL_OK && o.wheel_drag != 0.0f) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_down(e2e, x, high);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, x, low);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &lab, drag_within, &backward, H2_G99_E2E_NAV_MS,
                         "reverse drag bounded to one item", NULL);
  const h2_pal_result_t reverse_released = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = reverse_released;
  if (rc == H2_PAL_OK)
    rc = menu_at(e2e, H2_G99_MAIN_LIBRARY, "Library after reverse snap", &o);
  if (rc == H2_PAL_OK && o.wheel_drag != 0.0f) rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static h2_pal_result_t wifi_region(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_WIFI);
  h2_g99_observation_t observation;
  h2_g99_e2e_expect_t wifi =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_WIFI);
  wifi.editing = H2_G99_E2E_ANY;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "Wi-Fi page", &observation);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, observation.width * 4 / 5, observation.height / 2,
                         observation.width / 10, observation.height / 4, 300u,
                         true, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "right-side gesture stays on Wi-Fi", &observation);
  if (rc == H2_PAL_OK &&
      (observation.wheel_drag != 0.0f || observation.text_entry)) {
    h2_g99_e2e_fail(e2e, "Wi-Fi gesture escaped its right-side region");
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_WIFI);
  if (rc == H2_PAL_OK && observation.editing)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &page,
                                H2_G99_E2E_NAV_MS, "close Wi-Fi selection");
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &settings,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"wheel-drag", 0u, wheel_drag},
    {"wheel-limits", 0u, wheel_limits},
    {"wheel-follow", 0u, wheel_follow},
    {"wheel-restart", 0u, wheel_restart},
    {"wheel-flick", H2_G99_E2E_CAP_FRAME_BUDGET, wheel_flick},
    {"wheel-tap", 0u, wheel_tap},
    {"full-screen-drag", 0u, full_screen_drag},
    {"page-tap", 0u, page_tap},
    {"wifi-region", 0u, wifi_region},
    {"hover-ignored", 0u, hover_ignored},
    {"empty-frames", 0u, empty_frames},
    {"failed-reads", 0u, failed_reads},
};

const h2_g99_e2e_group_t h2_g99_e2e_touch_group = {
    "touch", kCases, sizeof(kCases) / sizeof(kCases[0])};
