#include "h2_g99_e2e_internal.h"

/* AI leaves the running Lua app for Library, and Main draws again. App pause
 * on screen-off is a known GizOS gap and is not asserted here. */
static h2_pal_result_t home_exits_app(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  h2_pal_result_t rc = h2_g99_e2e_press_until(
      e2e, H2_G99_COMPONENT_OK, &library, H2_G99_E2E_NAV_MS, "Library");
  if (rc != H2_PAL_OK) return rc;
  h2_g99_e2e_expect_t app = h2_g99_e2e_any();
  app.route = H2_G99_ROUTE_APP;
  app.lifecycle = H2_G99_LIFECYCLE_APP_RUNNING;
  rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &app, 5000u,
                              "Orbit running");
  /* The app keeps running; it does not end on its own. */
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_stays(e2e, &app, 1500u, "Orbit running");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &library, 5000u,
                                "Library after AI");
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &menu,
                                H2_G99_E2E_NAV_MS, "MAIN(Library)");
  return rc;
}

/* BOOT turns the screen off and on; any key wakes it without acting. */
static h2_pal_result_t screen_toggle(h2_g99_e2e_t* e2e) {
  h2_g99_e2e_expect_t off = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  off.screen_on = 0;
  const h2_g99_e2e_expect_t on = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  h2_pal_result_t rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN,
                                              &off, 1000u, "screen off");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_stays(e2e, &off, 500u, "screen off");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &on, 1000u,
                                "screen on");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &off, 1000u,
                                "screen off again");
  /* DOWN while off only wakes: the wheel stays on Library. */
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &on, 1000u,
                                "wake by DOWN");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_stays(e2e, &on, 500u, "woken menu");
  return rc;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"home-exits-app", 0u, home_exits_app},
    {"screen-toggle", 0u, screen_toggle},
};

const h2_g99_e2e_group_t h2_g99_e2e_keys_group = {
    "keys", kCases, sizeof(kCases) / sizeof(kCases[0])};
