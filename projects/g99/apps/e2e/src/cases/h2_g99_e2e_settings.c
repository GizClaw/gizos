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
                           "timeout focus", NULL);
  }
  if (rc != H2_PAL_OK) return rc;
  /* OK picks the focused choice and closes the list. */
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

static const h2_g99_e2e_case_t kCases[] = {
    {"persist-across-restart", 0u, persist_across_restart},
};

const h2_g99_e2e_group_t h2_g99_e2e_settings_group = {
    "settings", kCases, sizeof(kCases) / sizeof(kCases[0])};
