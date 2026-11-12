#include "h2_g99_e2e_internal.h"

/* Mirrors h2_g99_settings.h: values move in 10 % steps, brightness never
 * drops below 10 %, and there are five screen timeout choices. */
#define STEP_PERCENT 10
#define BRIGHTNESS_MIN 10
#define TIMEOUT_COUNT 5u
#define MAX_STEPS 12

typedef struct values {
  int volume;
  int brightness;
  int timeout;
} values_t;

typedef struct field_value {
  uint8_t item;
  int value;
} field_value_t;

static int field_of(const h2_g99_observation_t* o, uint8_t item) {
  switch (item) {
    case H2_G99_SETTING_VOLUME:
      return o->volume_percent;
    case H2_G99_SETTING_BRIGHTNESS:
      return o->brightness_percent;
    default:
      return o->screen_timeout_index;
  }
}

static bool has_value(const h2_g99_observation_t* o, const void* arg) {
  const field_value_t* f = (const field_value_t*)arg;
  return field_of(o, f->item) == f->value;
}

static bool edit_at(const h2_g99_observation_t* o, const void* arg) {
  return o->edit_index == *(const uint8_t*)arg;
}

static h2_g99_e2e_expect_t editing(uint8_t item) {
  h2_g99_e2e_expect_t e = h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item);
  e.editing = 1;
  return e;
}

/* On the settled `item` page: edits a percent value one step at a time. */
static h2_pal_result_t set_percent(h2_g99_e2e_t* e2e, uint8_t item,
                                   int target) {
  const h2_g99_e2e_expect_t edit = editing(item);
  h2_g99_observation_t o;
  h2_pal_result_t rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &edit,
                                              H2_G99_E2E_NAV_MS, "edit value");
  const int minimum = item == H2_G99_SETTING_BRIGHTNESS ? BRIGHTNESS_MIN : 0;
  for (int i = 0; rc == H2_PAL_OK && i < MAX_STEPS; ++i) {
    rc = h2_g99_e2e_wait(e2e, &edit, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "value editor", &o);
    const int current = field_of(&o, item);
    if (rc != H2_PAL_OK || current == target) break;
    /* UP raises the value, DOWN lowers it. */
    const bool up = current < target;
    int next = current + (up ? STEP_PERCENT : -STEP_PERCENT);
    if (next > 100) next = 100;
    if (next < minimum) next = minimum;
    const field_value_t expect = {item, next};
    rc =
        h2_g99_e2e_press(e2e, up ? H2_G99_COMPONENT_UP : H2_G99_COMPONENT_DOWN);
    if (rc == H2_PAL_OK)
      rc = h2_g99_e2e_wait(e2e, &edit, has_value, &expect, H2_G99_E2E_NAV_MS,
                           "value step", NULL);
  }
  if (rc == H2_PAL_OK && field_of(&o, item) != target) {
    h2_g99_e2e_fail(e2e, "setting %u stuck at %d, want %d", (unsigned)item,
                    field_of(&o, item), target);
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &page,
                                H2_G99_E2E_NAV_MS, "end value edit");
  return rc;
}

/* On the settled timeout page: picks choice `target` from the list. */
static h2_pal_result_t set_timeout(h2_g99_e2e_t* e2e, int target) {
  const uint8_t item = H2_G99_SETTING_SCREEN_TIMEOUT;
  const h2_g99_e2e_expect_t edit = editing(item);
  h2_pal_result_t rc = h2_g99_e2e_press_until(
      e2e, H2_G99_COMPONENT_OK, &edit, H2_G99_E2E_NAV_MS, "edit timeout");
  h2_g99_observation_t o;
  for (unsigned i = 0u; rc == H2_PAL_OK && i < TIMEOUT_COUNT; ++i) {
    rc = h2_g99_e2e_wait(e2e, &edit, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "timeout list", &o);
    if (rc != H2_PAL_OK || o.edit_index == (uint8_t)target) break;
    const uint8_t next = (uint8_t)((o.edit_index + 1u) % TIMEOUT_COUNT);
    rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_DOWN);
    if (rc == H2_PAL_OK)
      rc = h2_g99_e2e_wait(e2e, &edit, edit_at, &next, H2_G99_E2E_NAV_MS,
                           "timeout focus", &o);
    if (rc == H2_PAL_OK && o.screen_timeout_index != next) {
      h2_g99_e2e_fail(e2e, "timeout focus did not apply immediately");
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (rc != H2_PAL_OK) return rc;
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item);
  const field_value_t expect = {item, target};
  rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, has_value, &expect, H2_G99_E2E_NAV_MS,
                         "timeout chosen", NULL);
  return rc;
}

static h2_pal_result_t next_setting(h2_g99_e2e_t* e2e, uint8_t item) {
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item);
  return h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &page,
                                H2_G99_E2E_NAV_MS, "next setting");
}

/* From rest, sets all three values through Settings and returns to rest. */
static h2_pal_result_t set_values(h2_g99_e2e_t* e2e, const values_t* v) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_VOLUME);
  if (rc == H2_PAL_OK) rc = set_percent(e2e, H2_G99_SETTING_VOLUME, v->volume);
  if (rc == H2_PAL_OK) rc = next_setting(e2e, H2_G99_SETTING_BRIGHTNESS);
  if (rc == H2_PAL_OK)
    rc = set_percent(e2e, H2_G99_SETTING_BRIGHTNESS, v->brightness);
  if (rc == H2_PAL_OK) rc = next_setting(e2e, H2_G99_SETTING_SCREEN_TIMEOUT);
  if (rc == H2_PAL_OK) rc = set_timeout(e2e, v->timeout);
  const h2_g99_e2e_expect_t back = h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &back,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static void read_values(const h2_g99_observation_t* o, values_t* out) {
  out->volume = o->volume_percent;
  out->brightness = o->brightness_percent;
  out->timeout = o->screen_timeout_index;
}

/* Restarts Main and requires it to come back with `want`. */
static h2_pal_result_t expect_after_restart(h2_g99_e2e_t* e2e,
                                            const values_t* want) {
  h2_pal_result_t rc = h2_g99_e2e_main_restart(e2e);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait_rest(e2e, H2_G99_E2E_START_MS, "Main restart");
  h2_g99_observation_t o;
  if (rc != H2_PAL_OK || !h2_g99_e2e_observe(e2e, &o, NULL)) return rc;
  values_t got;
  read_values(&o, &got);
  if (got.volume != want->volume || got.brightness != want->brightness ||
      got.timeout != want->timeout) {
    h2_g99_e2e_fail(e2e,
                    "after restart volume=%d brightness=%d timeout=%d, want "
                    "%d/%d/%d",
                    got.volume, got.brightness, got.timeout, want->volume,
                    want->brightness, want->timeout);
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t persist_across_restart(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t o;
  if (!h2_g99_e2e_observe(e2e, &o, NULL)) return H2_PAL_ERR_INVALID_STATE;
  values_t original;
  read_values(&o, &original);
  /* One step away from each original value, staying in range. */
  values_t changed = original;
  changed.volume +=
      original.volume <= 100 - STEP_PERCENT ? STEP_PERCENT : -STEP_PERCENT;
  changed.brightness +=
      original.brightness <= 100 - STEP_PERCENT ? STEP_PERCENT : -STEP_PERCENT;
  /* Never pick 15 s: it could turn the screen off mid-case. */
  changed.timeout = original.timeout == 2 ? 3 : 2;
  h2_pal_result_t rc = set_values(e2e, &changed);
  if (rc == H2_PAL_OK) rc = expect_after_restart(e2e, &changed);
  /* Put the user's values back the same way, and prove they stuck. */
  h2_pal_result_t restore = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (restore == H2_PAL_OK) restore = set_values(e2e, &original);
  if (restore == H2_PAL_OK) restore = expect_after_restart(e2e, &original);
  return rc != H2_PAL_OK ? rc : restore;
}

typedef struct percent_range {
  uint8_t item;
  int minimum;
  int maximum;
} percent_range_t;

static bool percent_within(const h2_g99_observation_t* observation,
                           const void* argument) {
  const percent_range_t* range = argument;
  const int value = field_of(observation, range->item);
  return value >= range->minimum && value <= range->maximum &&
         observation->wheel_drag == 0.0f;
}

static int32_t slider_y(const h2_g99_observation_t* observation) {
  const int32_t short_side = observation->width < observation->height
                                 ? observation->width
                                 : observation->height;
  return observation->height / 2 + short_side * 3 / 20;
}

static h2_pal_result_t drag_slider(h2_g99_e2e_t* e2e, uint8_t item) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, item);
  h2_g99_observation_t observation = {0};
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item);
  const h2_g99_e2e_expect_t editor = editing(item);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "slider page", &observation);
  const int32_t width = observation.info_rect.x2 - observation.info_rect.x1;
  const int32_t left = observation.info_rect.x1 + width / 4;
  const int32_t right = observation.info_rect.x1 + width * 3 / 4;
  const int32_t height = slider_y(&observation);
  const field_value_t unchanged = {item, field_of(&observation, item)};
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_tap(e2e, right, observation.height / 10);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_stays(e2e, &page, 200u,
                          "background tap keeps direct slider ready");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, has_value, &unchanged, H2_G99_E2E_NAV_MS,
                         "background tap preserves value", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &editor,
                                H2_G99_E2E_NAV_MS, "begin value editing");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, observation.width / 10,
                         observation.height * 3 / 4, observation.width / 10,
                         observation.height / 2, 400u, true, NULL);
  const h2_g99_e2e_expect_t next =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, item + 1u);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &next, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "wheel leaves active value editor", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_UP, &page,
                                H2_G99_E2E_NAV_MS, "return to slider");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &editor,
                                H2_G99_E2E_NAV_MS, "edit before tapping");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_tap(e2e, left, height);
  const percent_range_t tapped = {item, 15, 40};
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, percent_within, &tapped, H2_G99_E2E_NAV_MS,
                         "tap releases value editor", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &editor,
                                H2_G99_E2E_NAV_MS, "edit before dragging");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_down(e2e, left, height);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_move(e2e, right, height);
  const percent_range_t moved = {item, 65, 95};
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_wait(e2e, &editor, percent_within, &moved, H2_G99_E2E_NAV_MS,
                        "value changes before release", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_move(e2e, observation.width / 10,
                               height - observation.height / 6);
  const field_value_t minimum = {
      item, item == H2_G99_SETTING_BRIGHTNESS ? BRIGHTNESS_MIN : 0};
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &editor, has_value, &minimum, H2_G99_E2E_NAV_MS,
                         "slider owns drag across the wheel", NULL);
  const h2_pal_result_t lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, has_value, &minimum, H2_G99_E2E_NAV_MS,
                         "slider release ends editing", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, observation.width / 10,
                         observation.height * 3 / 4, observation.width / 10,
                         observation.height / 2, 400u, true, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &next, has_value, &minimum, H2_G99_E2E_NAV_MS,
                         "wheel turns immediately after slider release", NULL);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  return rc;
}

static h2_pal_result_t slider_release(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t observation;
  if (!h2_g99_e2e_observe(e2e, &observation, NULL))
    return H2_PAL_ERR_INVALID_STATE;
  values_t original;
  read_values(&observation, &original);
  h2_pal_result_t rc = drag_slider(e2e, H2_G99_SETTING_VOLUME);
  if (rc == H2_PAL_OK) rc = drag_slider(e2e, H2_G99_SETTING_BRIGHTNESS);
  const values_t changed = {0, BRIGHTNESS_MIN, original.timeout};
  if (rc == H2_PAL_OK) rc = expect_after_restart(e2e, &changed);
  h2_pal_result_t restore = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (restore == H2_PAL_OK) restore = set_values(e2e, &original);
  return rc != H2_PAL_OK ? rc : restore;
}

static h2_pal_result_t slider_cancel(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t observation = {0};
  if (!h2_g99_e2e_observe(e2e, &observation, NULL))
    return H2_PAL_ERR_INVALID_STATE;
  values_t original;
  read_values(&observation, &original);
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_VOLUME);
  const h2_g99_e2e_expect_t editor = editing(H2_G99_SETTING_VOLUME);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &editor,
                                H2_G99_E2E_NAV_MS, "edit before interruption");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &editor, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "volume slider", &observation);
  const int32_t left =
      observation.info_rect.x1 +
      (observation.info_rect.x2 - observation.info_rect.x1) / 4;
  const int32_t height = slider_y(&observation);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_down(e2e, left, height);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_move(e2e, observation.info_rect.x2 - 1, height);
  const field_value_t loud = {H2_G99_SETTING_VOLUME, 100};
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &editor, has_value, &loud, H2_G99_E2E_NAV_MS,
                         "held slider reaches maximum", NULL);
  h2_g99_e2e_expect_t asleep = editor;
  asleep.screen_on = 0;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &asleep,
                                H2_G99_E2E_NAV_MS, "screen interrupts slider");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_move(e2e, observation.width / 10, height);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_SCREEN, &editor,
                                H2_G99_E2E_NAV_MS, "wake while finger held");
  const h2_pal_result_t lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &editor, has_value, &loud, H2_G99_E2E_NAV_MS,
                         "cancelled pointer cannot change volume", NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, observation.width / 10,
                         observation.height * 3 / 4, observation.width / 10,
                         observation.height / 2, 400u, true, NULL);
  const h2_g99_e2e_expect_t brightness =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_BRIGHTNESS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &brightness, has_value, &loud, H2_G99_E2E_NAV_MS,
                         "new pointer can turn the wheel", NULL);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  values_t changed = original;
  changed.volume = 100;
  if (rc == H2_PAL_OK) rc = expect_after_restart(e2e, &changed);
  h2_pal_result_t restore = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (restore == H2_PAL_OK) restore = set_values(e2e, &original);
  return rc != H2_PAL_OK ? rc : restore;
}

static h2_pal_result_t menu_items(h2_g99_e2e_t* e2e) {
  const uint8_t items[] = {
      H2_G99_SETTING_WIFI,       H2_G99_SETTING_VOLUME,
      H2_G99_SETTING_BRIGHTNESS, H2_G99_SETTING_SCREEN_TIMEOUT,
      H2_G99_SETTING_ABOUT,      H2_G99_SETTING_HAPTICS,
      H2_G99_SETTING_STORAGE,    H2_G99_SETTING_UPDATE,
      H2_G99_SETTING_ACCOUNT,    H2_G99_SETTING_CLOCK,
      H2_G99_SETTING_LANGUAGE,   H2_G99_SETTING_RESET,
  };
  const size_t count = sizeof(items) / sizeof(items[0]);
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_WIFI);
  for (size_t index = 1u; rc == H2_PAL_OK && index <= count; ++index) {
    const h2_g99_e2e_expect_t page =
        h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, items[index % count]);
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &page,
                                H2_G99_E2E_NAV_MS, "visible settings wheel");
  }
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  return rc;
}

typedef struct haptics_value {
  h2_g99_e2e_t* e2e;
  uint32_t enabled;
} haptics_value_t;

static bool haptics_saved(const h2_g99_observation_t* observation,
                          const void* argument) {
  (void)observation;
  const haptics_value_t* expected = argument;
  h2_pal_pref_namespace_t* name_space = NULL;
  int result = h2_pal_pref_open(expected->e2e->runtime->pref,
                                H2_G99_E2E_SETTINGS_NAMESPACE,
                                H2_PAL_PREF_OPEN_READ_ONLY, &name_space);
  if (result == H2_PAL_ERR_NOT_FOUND) return expected->enabled == 0u;
  if (result != H2_PAL_OK || name_space == NULL) return false;
  uint32_t value = 0u;
  result = name_space->get_u32(name_space, "haptics", &value);
  const int closed = name_space->close(name_space);
  return (result == H2_PAL_OK || result == H2_PAL_ERR_NOT_FOUND) &&
         closed == H2_PAL_OK && value == expected->enabled;
}

static h2_pal_result_t toggle_haptics(h2_g99_e2e_t* e2e, uint32_t enabled) {
  h2_g99_observation_t observation = {0};
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_HAPTICS);
  h2_pal_result_t result = h2_g99_e2e_wait(
      e2e, &page, NULL, NULL, H2_G99_E2E_NAV_MS, "haptics page", &observation);
  const int32_t center =
      (observation.info_rect.x1 + observation.info_rect.x2) / 2;
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_touch_down(e2e, center, observation.height / 3);
  if (result == H2_PAL_OK) result = h2_g99_e2e_touch_up(e2e);
  const haptics_value_t expected = {e2e, enabled};
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_wait(e2e, &page, haptics_saved, &expected,
                             H2_G99_E2E_NAV_MS, "haptics saved", NULL);
  return result;
}

static h2_pal_result_t haptics_switch(h2_g99_e2e_t* e2e) {
  haptics_value_t original = {e2e, 1u};
  original.enabled = haptics_saved(NULL, &original) ? 1u : 0u;
  h2_pal_result_t result = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_HAPTICS);
  if (result == H2_PAL_OK)
    result = toggle_haptics(e2e, original.enabled == 0u ? 1u : 0u);
  if (result == H2_PAL_OK) result = next_setting(e2e, H2_G99_SETTING_STORAGE);
  if (result == H2_PAL_OK) result = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (result == H2_PAL_OK) result = h2_g99_e2e_main_restart(e2e);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_wait_rest(e2e, H2_G99_E2E_START_MS, "haptics restart");
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_HAPTICS);
  if (result == H2_PAL_OK) result = toggle_haptics(e2e, original.enabled);
  if (result == H2_PAL_OK) result = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  return result;
}

static h2_pal_result_t timeout_wheel(h2_g99_e2e_t* e2e) {
  h2_g99_observation_t observation = {0};
  if (!h2_g99_e2e_observe(e2e, &observation, NULL))
    return H2_PAL_ERR_INVALID_STATE;
  values_t original;
  read_values(&observation, &original);
  h2_pal_result_t result =
      h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_SCREEN_TIMEOUT);
  if (result == H2_PAL_OK) result = set_timeout(e2e, 1);
  const h2_g99_e2e_expect_t page = h2_g99_e2e_page_ready(
      H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_SCREEN_TIMEOUT);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_wait(e2e, &page, NULL, NULL, H2_G99_E2E_NAV_MS,
                             "timeout wheel ready", &observation);
  const int32_t center =
      (observation.info_rect.x1 + observation.info_rect.x2) / 2;
  const int32_t row_height = observation.height / 10;
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_drag(
        e2e, center, observation.height / 2, observation.width / 10,
        observation.height / 2 - row_height * 9 / 10, 450u, true, NULL);
  const field_value_t minute = {H2_G99_SETTING_SCREEN_TIMEOUT, 2};
  if (result == H2_PAL_OK)
    result =
        h2_g99_e2e_wait(e2e, &page, has_value, &minute, H2_G99_E2E_NAV_MS,
                        "right timeout wheel selects without confirm", NULL);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_tap(e2e, center, observation.height / 2 + row_height);
  const field_value_t five_minutes = {H2_G99_SETTING_SCREEN_TIMEOUT, 3};
  if (result == H2_PAL_OK)
    result =
        h2_g99_e2e_wait(e2e, &page, has_value, &five_minutes, H2_G99_E2E_NAV_MS,
                        "timeout tap selects and snaps without confirm", NULL);
  if (result == H2_PAL_OK)
    result =
        h2_g99_e2e_drag(e2e, observation.width / 10, observation.height * 3 / 4,
                        center, observation.height / 2, 450u, true, NULL);
  const h2_g99_e2e_expect_t about =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_ABOUT);
  if (result == H2_PAL_OK)
    result = h2_g99_e2e_wait(e2e, &about, NULL, NULL, H2_G99_E2E_NAV_MS,
                             "left wheel leaves timeout", NULL);
  if (result == H2_PAL_OK) result = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  values_t changed = original;
  changed.timeout = 3;
  if (result == H2_PAL_OK) result = expect_after_restart(e2e, &changed);
  h2_pal_result_t restore = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (restore == H2_PAL_OK)
    restore = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_SCREEN_TIMEOUT);
  if (restore == H2_PAL_OK) restore = set_timeout(e2e, original.timeout);
  if (restore == H2_PAL_OK)
    restore = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  return result != H2_PAL_OK ? result : restore;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"menu-items", 0u, menu_items},
    {"haptics-switch", 0u, haptics_switch},
    {"timeout-wheel", 0u, timeout_wheel},
    {"slider-release", 0u, slider_release},
    {"slider-cancel", 0u, slider_cancel},
    {"persist-across-restart", 0u, persist_across_restart},
};

const h2_g99_e2e_group_t h2_g99_e2e_settings_group = {
    "settings", kCases, sizeof(kCases) / sizeof(kCases[0])};
