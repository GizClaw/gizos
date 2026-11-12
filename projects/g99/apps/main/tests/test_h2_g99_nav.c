#include "h2_g99_layout.h"
#include "h2_g99_nav.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static const h2_g99_action_t kAll[] = {H2_G99_ACTION_HOME, H2_G99_ACTION_SCREEN,
                                       H2_G99_ACTION_UP, H2_G99_ACTION_DOWN,
                                       H2_G99_ACTION_OK};

/* Boots and returns the time at which MAIN(Library) became ready. */
static uint64_t booted(h2_g99_nav_t* nav) {
  h2_g99_nav_init(nav, 1u, 1000u);
  CHECK(h2_g99_nav_tick(nav, 1000u + H2_G99_BOOT_MS) == H2_G99_NAV_CHANGED);
  return 1000u + H2_G99_BOOT_MS;
}

/* Starts one transition and completes it; returns the completion time. */
static uint64_t step(h2_g99_nav_t* nav, h2_g99_action_t action, uint64_t t,
                     h2_g99_nav_result_t expected) {
  CHECK(h2_g99_nav_input(nav, action, t) == expected);
  if (nav->anim == H2_G99_TRANSITION_NONE) return t;
  const uint64_t end = t + nav->anim_duration_ms;
  CHECK(h2_g99_nav_tick(nav, end - 1u) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_tick(nav, end) == H2_G99_NAV_CHANGED);
  return end;
}

static void test_boot_gates_every_input(void) {
  h2_g99_nav_t nav;
  h2_g99_nav_init(&nav, 1u, 1000u);
  CHECK(nav.route == H2_G99_ROUTE_BOOT);
  CHECK(nav.anim == H2_G99_TRANSITION_BOOT);
  CHECK(!h2_g99_nav_input_ready(&nav));
  for (uint64_t t = 1000u; t < 1000u + H2_G99_BOOT_MS; t += 100u) {
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i)
      CHECK(h2_g99_nav_input(&nav, kAll[i], t) == H2_G99_NAV_IGNORED);
    CHECK(h2_g99_nav_tick(&nav, t) == H2_G99_NAV_IGNORED);
  }
  CHECK(nav.screen_on);
  CHECK(h2_g99_nav_tick(&nav, 1000u + H2_G99_BOOT_MS - 1u) ==
        H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_tick(&nav, 1000u + H2_G99_BOOT_MS) == H2_G99_NAV_CHANGED);
  /* Nothing pressed during boot is replayed: still MAIN(Library), idle. */
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  CHECK(nav.main_sel == H2_G99_MAIN_LIBRARY);
  CHECK(nav.anim == H2_G99_TRANSITION_NONE);
  CHECK(h2_g99_nav_input_ready(&nav));
}

static void test_enter_back_restores_selection(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.anim == H2_G99_TRANSITION_ENTER);
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  CHECK(nav.to_route == H2_G99_ROUTE_LIBRARY);
  CHECK(nav.anim_duration_ms == H2_G99_PAGE_TRANSITION_MS);
  /* TRANSITION ignores every key, including screen and back. */
  for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i)
    CHECK(h2_g99_nav_input(&nav, kAll[i], t + 300u) == H2_G99_NAV_IGNORED);
  CHECK(nav.screen_on);
  h2_g99_observation_t o = {0};
  h2_g99_nav_observe(&nav, &o);
  CHECK(o.transition == H2_G99_TRANSITION_ENTER);
  CHECK(o.target_route == H2_G99_ROUTE_LIBRARY);
  CHECK(!o.input_ready);
  CHECK(h2_g99_nav_tick(&nav, t + 599u) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_tick(&nav, t + 600u) == H2_G99_NAV_CHANGED);
  t += 600u;
  CHECK(nav.route == H2_G99_ROUTE_LIBRARY);
  CHECK(nav.sub_sel == 0u);
  t = step(&nav, H2_G99_ACTION_HOME, t, H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  CHECK(nav.main_sel == H2_G99_MAIN_LIBRARY);
  /* Settings: rotate twice, enter, come back to the same item. */
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_SETTINGS);
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_SETTINGS);
  CHECK(nav.sub_sel == 0u);
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == 1u);
  t = step(&nav, H2_G99_ACTION_HOME, t, H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  CHECK(nav.main_sel == H2_G99_MAIN_SETTINGS);
  /* Re-entering always starts on the first item. */
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == 0u);
}

static void test_rotation_wraps_and_gates(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.anim == H2_G99_TRANSITION_ROTATE);
  CHECK(nav.anim_duration_ms == H2_G99_ROTATE_MS);
  CHECK(nav.rotate_dir == -1);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t + 100u) ==
        H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_progress(&nav, t + 110u) > 0.49f &&
        h2_g99_nav_progress(&nav, t + 110u) < 0.51f);
  CHECK(h2_g99_nav_tick(&nav, t + H2_G99_ROTATE_MS) == H2_G99_NAV_CHANGED);
  t += H2_G99_ROTATE_MS;
  CHECK(nav.main_sel == H2_G99_MAIN_STORE);
  t = step(&nav, H2_G99_ACTION_UP, t, H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_ME);
  t = step(&nav, H2_G99_ACTION_UP, t, H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_SETTINGS);
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_ME);
  /* Entries that are not designed yet only show their two lines. */
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) == H2_G99_NAV_IGNORED);
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  CHECK(!h2_g99_nav_main_enterable(H2_G99_MAIN_STORE));
  CHECK(!h2_g99_nav_main_enterable(H2_G99_MAIN_LAB));
  CHECK(h2_g99_nav_main_enterable(H2_G99_MAIN_LIBRARY));
  CHECK(h2_g99_nav_main_enterable(H2_G99_MAIN_SETTINGS));
}

static void test_single_item_library_does_not_rotate(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t) == H2_G99_NAV_IGNORED);
  CHECK(nav.anim == H2_G99_TRANSITION_NONE);
}

/* A released drag turns several items, or none, from where the finger left
 * the wheel; a finger on the turning wheel completes the rotation. */
static void test_drag_rotation(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  CHECK(h2_g99_nav_rotate_from(&nav, 0, 0.0f, t) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_rotate_from(&nav, -2, -1.6f, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.anim == H2_G99_TRANSITION_ROTATE);
  CHECK(nav.from_sel == H2_G99_MAIN_LIBRARY);
  CHECK(nav.to_sel == H2_G99_MAIN_ME);
  CHECK(nav.rotate_dir == -2 && nav.rotate_from == -1.6f);
  /* Gated while it turns, like a key rotation. */
  CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.5f, t + 1u) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_finish_rotate(&nav, t + 1u));
  CHECK(nav.anim == H2_G99_TRANSITION_NONE && nav.main_sel == H2_G99_MAIN_ME);
  CHECK(nav.rotate_dir == 0 && nav.rotate_from == 0.0f);
  CHECK(!h2_g99_nav_finish_rotate(&nav, t + 2u));
  /* Wrapping backwards from the first item, then settling back in place. */
  CHECK(h2_g99_nav_rotate_from(&nav, -1, -0.7f, t + 2u) == H2_G99_NAV_CHANGED);
  CHECK(nav.to_sel == H2_G99_MAIN_SETTINGS);
  t = t + 2u + H2_G99_ROTATE_MS;
  CHECK(h2_g99_nav_tick(&nav, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_SETTINGS);
  CHECK(h2_g99_nav_rotate_from(&nav, 0, 0.2f, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.to_sel == H2_G99_MAIN_SETTINGS);
  t += H2_G99_ROTATE_MS;
  CHECK(h2_g99_nav_tick(&nav, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.main_sel == H2_G99_MAIN_SETTINGS);
  /* Page transitions are not caught, and a single item never turns. */
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_SETTINGS);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) == H2_G99_NAV_CHANGED);
  CHECK(!h2_g99_nav_finish_rotate(&nav, t + 1u));
  t += H2_G99_PAGE_TRANSITION_MS;
  CHECK(h2_g99_nav_tick(&nav, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_MAIN);
  /* Turn to Library and enter it: one item, nothing to turn. */
  CHECK(h2_g99_nav_rotate_from(&nav, -2, -2.0f, t) == H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_finish_rotate(&nav, t));
  CHECK(nav.main_sel == H2_G99_MAIN_LIBRARY);
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(nav.route == H2_G99_ROUTE_LIBRARY);
  CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.8f, t) == H2_G99_NAV_IGNORED);
}

static void test_app_launch_and_home(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  t = step(&nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_LAUNCH_APP);
  CHECK(nav.route == H2_G99_ROUTE_APP);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t) ==
        H2_G99_NAV_FORWARD_TO_APP);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) ==
        H2_G99_NAV_FORWARD_TO_APP);
  /* Apps never time the screen out on their own. */
  CHECK(h2_g99_nav_tick(&nav, t + 10u * 60u * 1000u) == H2_G99_NAV_IGNORED);
  CHECK(nav.screen_on);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) == H2_G99_NAV_EXIT_APP);
  CHECK(nav.route == H2_G99_ROUTE_LIBRARY);
  CHECK(nav.sub_sel == 0u);
  h2_g99_nav_t empty;
  h2_g99_nav_init(&empty, 0u, 0u);
  (void)h2_g99_nav_tick(&empty, H2_G99_BOOT_MS);
  (void)step(&empty, H2_G99_ACTION_OK, H2_G99_BOOT_MS, H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_input(&empty, H2_G99_ACTION_OK, 5000u) ==
        H2_G99_NAV_IGNORED);
}

static void test_screen_toggle_and_timeout(void) {
  h2_g99_nav_t nav;
  uint64_t t = booted(&nav);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_SCREEN, t) ==
        H2_G99_NAV_SCREEN_OFF);
  CHECK(!nav.screen_on);
  h2_g99_observation_t o = {0};
  h2_g99_nav_observe(&nav, &o);
  CHECK(!o.screen_on);
  /* The waking press does nothing else. */
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t + 10u) ==
        H2_G99_NAV_SCREEN_ON);
  CHECK(nav.anim == H2_G99_TRANSITION_NONE);
  CHECK(nav.main_sel == H2_G99_MAIN_LIBRARY);
  h2_g99_nav_set_screen_timeout(&nav, 15000u, t + 10u);
  CHECK(h2_g99_nav_tick(&nav, t + 10u + 14999u) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_tick(&nav, t + 10u + 15000u) == H2_G99_NAV_SCREEN_OFF);
  CHECK(!nav.screen_on);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_SCREEN, t + 20000u) ==
        H2_G99_NAV_SCREEN_ON);
  /* Input restarts the idle timer. */
  t += 20000u;
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_tick(&nav, t + 14999u) == H2_G99_NAV_IGNORED);
  h2_g99_nav_set_screen_timeout(&nav, 0u, t);
  CHECK(h2_g99_nav_tick(&nav, t + 3600u * 1000u) == H2_G99_NAV_IGNORED);
  CHECK(nav.screen_on);
  /* The screen key is ignored during a transition. */
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t) == H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_SCREEN, t + 10u) ==
        H2_G99_NAV_IGNORED);
  CHECK(nav.screen_on);
}

static uint64_t to_settings(h2_g99_nav_t* nav, uint8_t item) {
  uint64_t t = booted(nav);
  t = step(nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  t = step(nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  t = step(nav, H2_G99_ACTION_OK, t, H2_G99_NAV_CHANGED);
  const uint8_t position =
      h2_g99_nav_wheel_position(H2_G99_ROUTE_SETTINGS, item);
  for (uint8_t i = 0u; i < position; ++i)
    t = step(nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(nav->route == H2_G99_ROUTE_SETTINGS);
  CHECK(nav->sub_sel == item);
  return t;
}

static void test_settings_value_editor(void) {
  h2_g99_nav_t nav;
  const uint64_t t = to_settings(&nav, H2_G99_SETTING_VOLUME);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_EDIT_BEGIN);
  CHECK(nav.editing);
  h2_g99_nav_set_edit_count(&nav, 0u, 0u);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t) ==
        H2_G99_NAV_EDIT_INCREASE);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t) ==
        H2_G99_NAV_EDIT_DECREASE);
  CHECK(nav.anim == H2_G99_TRANSITION_NONE);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) ==
        H2_G99_NAV_EDIT_ACTIVATE);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) == H2_G99_NAV_EDIT_END);
  CHECK(!nav.editing);
  CHECK(nav.route == H2_G99_ROUTE_SETTINGS);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) == H2_G99_NAV_CHANGED);
  CHECK(nav.anim == H2_G99_TRANSITION_BACK);
}

static void test_settings_list_editor(void) {
  h2_g99_nav_t nav;
  const uint64_t t = to_settings(&nav, H2_G99_SETTING_SCREEN_TIMEOUT);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_EDIT_BEGIN);
  h2_g99_nav_set_edit_count(&nav, 5u, 1u);
  CHECK(nav.edit_index == 1u);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t) == H2_G99_NAV_EDIT_MOVED);
  CHECK(nav.edit_index == 2u);
  (void)h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t);
  (void)h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t);
  CHECK(nav.edit_index == 0u);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t) == H2_G99_NAV_EDIT_MOVED);
  CHECK(nav.edit_index == 4u);
  h2_g99_observation_t o = {0};
  h2_g99_nav_observe(&nav, &o);
  CHECK(o.editing && o.edit_index == 4u);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) ==
        H2_G99_NAV_EDIT_ACTIVATE);
  h2_g99_nav_set_edit_count(&nav, 2u, 9u);
  CHECK(nav.edit_index == 1u);
  h2_g99_nav_end_edit(&nav);
  CHECK(!nav.editing && nav.edit_count == 0u);
}

static void test_settings_unimplemented_items(void) {
  for (uint8_t item = 0u; item < H2_G99_SETTING_COUNT; ++item) {
    CHECK(h2_g99_nav_setting_implemented(item) ==
          (item <= H2_G99_SETTING_ABOUT || item == H2_G99_SETTING_HAPTICS ||
           item == H2_G99_SETTING_STORAGE));
    CHECK(h2_g99_nav_setting_editable(item) == (item < H2_G99_SETTING_ABOUT));
  }
  h2_g99_nav_t nav;
  uint64_t t = to_settings(&nav, H2_G99_SETTING_ABOUT);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_IGNORED);
  t = step(&nav, H2_G99_ACTION_DOWN, t, H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == H2_G99_SETTING_HAPTICS);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_IGNORED);
  CHECK(!nav.editing);
  /* The settings wheel wraps across its visible items. */
  h2_g99_nav_t wrap;
  const uint64_t w = to_settings(&wrap, 0u);
  (void)step(&wrap, H2_G99_ACTION_UP, w, H2_G99_NAV_CHANGED);
  CHECK(wrap.sub_sel == H2_G99_SETTING_RESET);
}

static void test_password_entry(void) {
  h2_g99_nav_t nav;
  const uint64_t t = to_settings(&nav, H2_G99_SETTING_WIFI);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_EDIT_BEGIN);
  h2_g99_nav_set_edit_count(&nav, 3u, 0u);
  (void)h2_g99_nav_input(&nav, H2_G99_ACTION_DOWN, t);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) ==
        H2_G99_NAV_EDIT_ACTIVATE);
  h2_g99_nav_set_text_entry(&nav, true);
  CHECK(nav.text_entry);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_UP, t) == H2_G99_NAV_IGNORED);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, t) == H2_G99_NAV_TEXT_SUBMIT);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_HOME, t) ==
        H2_G99_NAV_TEXT_CANCEL);
  CHECK(!nav.text_entry);
  CHECK(nav.editing && nav.edit_index == 1u);
  /* The screen never times out while a password is typed. */
  h2_g99_nav_set_text_entry(&nav, true);
  h2_g99_nav_set_screen_timeout(&nav, 15000u, t);
  CHECK(h2_g99_nav_tick(&nav, t + 60000u) == H2_G99_NAV_IGNORED);
  /* Text entry requires an editor. */
  h2_g99_nav_end_edit(&nav);
  h2_g99_nav_set_text_entry(&nav, true);
  CHECK(!nav.text_entry);
}

static void test_wifi_left_wheel(void) {
  h2_g99_nav_t nav;
  uint64_t moment = to_settings(&nav, H2_G99_SETTING_WIFI);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, moment) ==
        H2_G99_NAV_EDIT_BEGIN);
  h2_g99_nav_set_edit_count(&nav, 4u, 2u);
  CHECK(h2_g99_nav_rotate_from(&nav, 0, 0.2f, moment) == H2_G99_NAV_CHANGED);
  moment += H2_G99_ROTATE_MS;
  CHECK(h2_g99_nav_tick(&nav, moment) == H2_G99_NAV_CHANGED);
  CHECK(nav.editing && nav.edit_index == 2u);
  CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.7f, moment) == H2_G99_NAV_CHANGED);
  CHECK(!nav.editing && nav.edit_count == 0u);
  moment += H2_G99_ROTATE_MS;
  CHECK(h2_g99_nav_tick(&nav, moment) == H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == H2_G99_SETTING_VOLUME);
  CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, moment) ==
        H2_G99_NAV_EDIT_BEGIN);
  CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.7f, moment) == H2_G99_NAV_CHANGED);
  CHECK(!nav.editing);
}

static void test_slider_editor_wheel(void) {
  const uint8_t items[] = {H2_G99_SETTING_VOLUME, H2_G99_SETTING_BRIGHTNESS,
                           H2_G99_SETTING_SCREEN_TIMEOUT};
  for (size_t index = 0u; index < sizeof(items) / sizeof(items[0]); ++index) {
    h2_g99_nav_t nav;
    uint64_t moment = to_settings(&nav, items[index]);
    CHECK(h2_g99_nav_input(&nav, H2_G99_ACTION_OK, moment) ==
          H2_G99_NAV_EDIT_BEGIN);
    CHECK(h2_g99_nav_rotate_from(&nav, 0, 0.2f, moment) == H2_G99_NAV_CHANGED);
    moment += H2_G99_ROTATE_MS;
    CHECK(h2_g99_nav_tick(&nav, moment) == H2_G99_NAV_CHANGED);
    CHECK(nav.editing && nav.sub_sel == items[index]);
    CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.7f, moment) == H2_G99_NAV_CHANGED);
    CHECK(!nav.editing);
    moment += H2_G99_ROTATE_MS;
    CHECK(h2_g99_nav_tick(&nav, moment) == H2_G99_NAV_CHANGED);
    CHECK(nav.sub_sel == items[index] + 1u);
  }
}

static void test_settings_skips_input(void) {
  h2_g99_nav_t nav;
  uint64_t moment = to_settings(&nav, H2_G99_SETTING_HAPTICS);
  CHECK(h2_g99_nav_route_count(&nav, H2_G99_ROUTE_SETTINGS) == 12u);
  moment = step(&nav, H2_G99_ACTION_DOWN, moment, H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == H2_G99_SETTING_STORAGE);
  moment = step(&nav, H2_G99_ACTION_UP, moment, H2_G99_NAV_CHANGED);
  CHECK(nav.sub_sel == H2_G99_SETTING_HAPTICS);
  CHECK(h2_g99_nav_rotate_from(&nav, 1, 0.8f, moment) == H2_G99_NAV_CHANGED);
  CHECK(h2_g99_nav_finish_rotate(&nav, moment));
  CHECK(nav.sub_sel == H2_G99_SETTING_STORAGE);
  bool visited[H2_G99_SETTING_COUNT] = {false};
  for (unsigned index = 0u; index < 12u; ++index) {
    CHECK(nav.sub_sel != H2_G99_SETTING_INPUT);
    visited[nav.sub_sel] = true;
    moment = step(&nav, H2_G99_ACTION_DOWN, moment, H2_G99_NAV_CHANGED);
  }
  CHECK(nav.sub_sel == H2_G99_SETTING_STORAGE);
  for (unsigned item = 0u; item < H2_G99_SETTING_COUNT; ++item)
    CHECK(visited[item] == (item != H2_G99_SETTING_INPUT));
}

int main(void) {
  test_settings_skips_input();
  test_slider_editor_wheel();
  test_wifi_left_wheel();
  test_boot_gates_every_input();
  test_enter_back_restores_selection();
  test_rotation_wraps_and_gates();
  test_single_item_library_does_not_rotate();
  test_drag_rotation();
  test_app_launch_and_home();
  test_screen_toggle_and_timeout();
  test_settings_value_editor();
  test_settings_list_editor();
  test_settings_unimplemented_items();
  test_password_entry();
  if (failures != 0) {
    fprintf(stderr, "%d navigation check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
