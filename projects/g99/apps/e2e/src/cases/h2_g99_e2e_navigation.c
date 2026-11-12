#include "h2_g99_e2e_internal.h"

static bool in_transition(const h2_g99_observation_t* o, const void* arg) {
  const int kind = *(const int*)arg;
  return (int)o->transition == kind && !o->input_ready;
}

/* Starts a page transition with `key`, proves that a key pressed during it
 * is ignored, and waits for the settled `result`. */
static h2_pal_result_t transition(h2_g99_e2e_t* e2e,
                                  h2_runtime_component_id_t key, int kind,
                                  h2_g99_route_t target,
                                  const h2_g99_e2e_expect_t* result,
                                  const char* what) {
  h2_pal_result_t rc = h2_g99_e2e_press(e2e, key);
  h2_g99_observation_t o;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, NULL, in_transition, &kind, 1000u, what, &o);
  if (rc != H2_PAL_OK) return rc;
  if (o.target_route != target) {
    h2_g99_e2e_fail(e2e, "%s targets route %d", what, (int)o.target_route);
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* DOWN during the 600 ms transition must not rotate the new page. */
  rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_DOWN);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_g99_e2e_wait(e2e, result, NULL, NULL, H2_G99_E2E_NAV_MS, what, NULL);
  if (rc != H2_PAL_OK) return rc;
  return h2_g99_e2e_stays(e2e, result, 400u, what);
}

static h2_pal_result_t library_enter_back(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  h2_pal_result_t rc =
      transition(e2e, H2_G99_COMPONENT_OK, H2_G99_TRANSITION_ENTER,
                 H2_G99_ROUTE_LIBRARY, &library, "enter Library");
  if (rc != H2_PAL_OK) return rc;
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  return transition(e2e, H2_G99_COMPONENT_HOME, H2_G99_TRANSITION_BACK,
                    H2_G99_ROUTE_MAIN, &menu, "AI back from Library");
}

static h2_pal_result_t settings_enter_back(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_SETTINGS);
  if (rc != H2_PAL_OK) return rc;
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, 0);
  rc = transition(e2e, H2_G99_COMPONENT_OK, H2_G99_TRANSITION_ENTER,
                  H2_G99_ROUTE_SETTINGS, &settings, "enter Settings");
  if (rc != H2_PAL_OK) return rc;
  /* Move inside Settings so returning must restore the main selection, not
   * the settings one. */
  const h2_g99_e2e_expect_t second =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, 1);
  rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &second,
                              H2_G99_E2E_NAV_MS, "settings rotation");
  if (rc != H2_PAL_OK) return rc;
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  rc = transition(e2e, H2_G99_COMPONENT_HOME, H2_G99_TRANSITION_BACK,
                  H2_G99_ROUTE_MAIN, &menu, "AI back from Settings");
  if (rc != H2_PAL_OK) return rc;
  /* Entering again starts on the first item. */
  rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &settings,
                              H2_G99_E2E_NAV_MS, "re-enter Settings");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &menu,
                                H2_G99_E2E_NAV_MS, "AI back again");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

/* 圈子, 应用中心 and 实验室 show their name and status only. */
static h2_pal_result_t non_enterable(h2_g99_e2e_t* e2e) {
  static const uint8_t items[] = {H2_G99_MAIN_ME, H2_G99_MAIN_STORE,
                                  H2_G99_MAIN_LAB};
  for (size_t i = 0u; i < sizeof(items) / sizeof(items[0]); ++i) {
    h2_pal_result_t rc = h2_g99_e2e_main_select(e2e, items[i]);
    if (rc == H2_PAL_OK) rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
    if (rc != H2_PAL_OK) return rc;
    const h2_g99_e2e_expect_t still = h2_g99_e2e_main_ready(items[i]);
    rc = h2_g99_e2e_stays(e2e, &still, 700u, "main menu after OK");
    if (rc != H2_PAL_OK) return rc;
  }
  return h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
}

static h2_pal_result_t open_library_menu(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t observation = {0};
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  h2_pal_result_t result =
      h2_g99_e2e_wait(e2e, &library, NULL, NULL, H2_G99_E2E_NAV_MS,
                      "library menu ready", &observation);
  const int32_t short_side = observation.width < observation.height
                                 ? observation.width
                                 : observation.height;
  if (result == H2_PAL_OK)
    result =
        h2_g99_e2e_tap(e2e, observation.info_rect.x2 - short_side * 7 / 120,
                       observation.height / 2);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_stays(e2e, &library, 400u, "drawer opens on Library");
  return result;
}

static h2_pal_result_t library_menu(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  h2_pal_result_t result = h2_g99_e2e_press_until(
      e2e, H2_G99_COMPONENT_OK, &library, H2_G99_E2E_NAV_MS, "Library details");
  if (result == H2_PAL_OK) result = open_library_menu(e2e);
  if (result == H2_PAL_OK) result = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_stays(e2e, &library, 300u,
                              "drawer blocks background game activation");
  h2_g99_observation_t observation = {0};
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_wait(e2e, &library, NULL, NULL, H2_G99_E2E_NAV_MS,
                             "drawer geometry", &observation);
  if (result == H2_PAL_OK)
    result =
        h2_g99_e2e_tap(e2e, observation.width / 10, observation.height / 2);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_stays(e2e, &library, 300u,
                              "outside tap closes without launching");
  if (result == H2_PAL_OK) result = open_library_menu(e2e);
  h2_g99_e2e_expect_t off = library;
  off.screen_on = 0;
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &off,
                                    H2_G99_E2E_NAV_MS,
                                    "screen off dismisses drawer");
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &library,
                                    H2_G99_E2E_NAV_MS, "wake Library");
  h2_g99_e2e_expect_t app = h2_g99_e2e_any();
  app.route = H2_G99_ROUTE_APP;
  app.lifecycle = H2_G99_LIFECYCLE_APP_RUNNING;
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &app,
                                    H2_G99_E2E_NAV_MS,
                                    "game launches after drawer dismissal");
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &library,
                                    H2_G99_E2E_NAV_MS, "return from game");
  if (result == H2_PAL_OK) result = open_library_menu(e2e);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_HOME);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_stays(e2e, &library, 300u,
                              "AI closes the drawer before leaving Library");
  if (result == H2_PAL_OK) result = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  return result;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"library-menu", 0u, library_menu},
    {"library-enter-back", 0u, library_enter_back},
    {"settings-enter-back", 0u, settings_enter_back},
    {"non-enterable", 0u, non_enterable},
};

const h2_g99_e2e_group_t h2_g99_e2e_navigation_group = {
    "navigation", kCases, sizeof(kCases) / sizeof(kCases[0])};
