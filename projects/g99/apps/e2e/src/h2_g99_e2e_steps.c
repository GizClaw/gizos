#include "h2_g99_e2e_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

h2_g99_e2e_expect_t h2_g99_e2e_any(void) {
  const h2_g99_e2e_expect_t any = {
      H2_G99_E2E_ANY, H2_G99_E2E_ANY, H2_G99_E2E_ANY,
      H2_G99_E2E_ANY, H2_G99_E2E_ANY, H2_G99_E2E_ANY,
      H2_G99_E2E_ANY, H2_G99_E2E_ANY, H2_G99_E2E_ANY,
  };
  return any;
}

h2_g99_e2e_expect_t h2_g99_e2e_main_ready(int selection) {
  h2_g99_e2e_expect_t expect = h2_g99_e2e_page_ready(H2_G99_ROUTE_MAIN, 0);
  expect.main_selection = selection;
  return expect;
}

h2_g99_e2e_expect_t h2_g99_e2e_page_ready(h2_g99_route_t route,
                                          int sub_selection) {
  h2_g99_e2e_expect_t expect = h2_g99_e2e_any();
  expect.lifecycle = H2_G99_LIFECYCLE_RUNNING;
  expect.route = (int)route;
  expect.sub_selection = sub_selection;
  expect.transition = H2_G99_TRANSITION_NONE;
  expect.input_ready = 1;
  expect.screen_on = 1;
  expect.editing = 0;
  expect.text_entry = 0;
  return expect;
}

static bool field(int expected, int actual) {
  return expected == H2_G99_E2E_ANY || expected == actual;
}

bool h2_g99_e2e_matches(const h2_g99_observation_t* o,
                        const h2_g99_e2e_expect_t* e) {
  return field(e->lifecycle, (int)o->lifecycle) &&
         field(e->route, (int)o->route) &&
         field(e->main_selection, o->main_selection) &&
         field(e->sub_selection, o->sub_selection) &&
         field(e->transition, (int)o->transition) &&
         field(e->input_ready, o->input_ready ? 1 : 0) &&
         field(e->screen_on, o->screen_on ? 1 : 0) &&
         field(e->editing, o->editing ? 1 : 0) &&
         field(e->text_entry, o->text_entry ? 1 : 0);
}

bool h2_g99_e2e_at_rest(const h2_g99_observation_t* o) {
  const h2_g99_e2e_expect_t rest = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  return h2_g99_e2e_matches(o, &rest);
}

static void describe(const h2_g99_observation_t* o, char* out, size_t size) {
  (void)snprintf(out, size,
                 "lifecycle=%d route=%d target=%d transition=%d main=%u "
                 "sub=%u ready=%d screen=%d editing=%d edit=%u text=%d",
                 (int)o->lifecycle, (int)o->route, (int)o->target_route,
                 (int)o->transition, o->main_selection, o->sub_selection,
                 o->input_ready ? 1 : 0, o->screen_on ? 1 : 0,
                 o->editing ? 1 : 0, o->edit_index, o->text_entry ? 1 : 0);
}

void h2_g99_e2e_fail(h2_g99_e2e_t* e2e, const char* format, ...) {
  if (e2e->detail[0] != '\0') return; /* The first failed step wins. */
  va_list args;
  va_start(args, format);
  (void)vsnprintf(e2e->detail, sizeof(e2e->detail), format, args);
  va_end(args);
}

void h2_g99_e2e_errorf(h2_g99_e2e_t* e2e, h2_pal_result_t rc,
                       const char* what) {
  e2e->error = true;
  h2_g99_e2e_fail(e2e, "%s rc=%d", what, (int)rc);
}

static void fail_with_state(h2_g99_e2e_t* e2e, const char* verb,
                            const char* what, uint32_t ms) {
  h2_g99_observation_t o;
  char state[160] = "no observation";
  if (h2_g99_e2e_observe(e2e, &o, NULL)) describe(&o, state, sizeof(state));
  if (!h2_g99_e2e_main_running(e2e))
    (void)snprintf(state, sizeof(state), "Main exited");
  h2_g99_e2e_fail(e2e, "%s %s after %u ms: %s", verb, what, (unsigned)ms,
                  state);
}

h2_pal_result_t h2_g99_e2e_wait(h2_g99_e2e_t* e2e,
                                const h2_g99_e2e_expect_t* expect,
                                h2_g99_e2e_pred_t pred, const void* arg,
                                uint32_t timeout_ms, const char* what,
                                h2_g99_observation_t* out) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_observation_t o;
    if (h2_g99_e2e_observe(e2e, &o, NULL) &&
        (expect == NULL || h2_g99_e2e_matches(&o, expect)) &&
        (pred == NULL || pred(&o, arg))) {
      if (out != NULL) *out = o;
      return H2_PAL_OK;
    }
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (elapsed >= timeout_ms || !h2_g99_e2e_main_running(e2e) ||
        h2_g99_e2e_aborted(e2e)) {
      fail_with_state(e2e, "timeout waiting for", what, (uint32_t)elapsed);
      return H2_PAL_ERR_TIMEOUT;
    }
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

h2_pal_result_t h2_g99_e2e_stays(h2_g99_e2e_t* e2e,
                                 const h2_g99_e2e_expect_t* expect,
                                 uint32_t duration_ms, const char* what) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    h2_g99_observation_t o;
    const uint64_t elapsed = h2_g99_e2e_now_ms(e2e) - start;
    if (!h2_g99_e2e_observe(e2e, &o, NULL) || !h2_g99_e2e_matches(&o, expect)) {
      fail_with_state(e2e, "left", what, (uint32_t)elapsed);
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (elapsed >= duration_ms) return H2_PAL_OK;
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

h2_pal_result_t h2_g99_e2e_press_until(h2_g99_e2e_t* e2e,
                                       h2_runtime_component_id_t component,
                                       const h2_g99_e2e_expect_t* expect,
                                       uint32_t timeout_ms, const char* what) {
  const h2_pal_result_t rc = h2_g99_e2e_press(e2e, component);
  if (rc != H2_PAL_OK) return rc;
  return h2_g99_e2e_wait(e2e, expect, NULL, NULL, timeout_ms, what, NULL);
}

h2_pal_result_t h2_g99_e2e_main_select(h2_g99_e2e_t* e2e, uint8_t item) {
  for (int step = 0; step < (int)H2_G99_MAIN_COUNT; ++step) {
    h2_g99_observation_t o;
    const h2_g99_e2e_expect_t ready = h2_g99_e2e_main_ready(H2_G99_E2E_ANY);
    h2_pal_result_t rc = h2_g99_e2e_wait(e2e, &ready, NULL, NULL,
                                         H2_G99_E2E_NAV_MS, "main menu", &o);
    if (rc != H2_PAL_OK) return rc;
    if (o.main_selection == item) return H2_PAL_OK;
    /* Turn the cyclic wheel the short way round. */
    const int forward =
        ((int)item - (int)o.main_selection + (int)H2_G99_MAIN_COUNT) %
        (int)H2_G99_MAIN_COUNT;
    const bool down = forward <= (int)H2_G99_MAIN_COUNT / 2;
    const int next =
        ((int)o.main_selection + (down ? 1 : -1) + (int)H2_G99_MAIN_COUNT) %
        (int)H2_G99_MAIN_COUNT;
    const h2_g99_e2e_expect_t moved = h2_g99_e2e_main_ready(next);
    rc = h2_g99_e2e_press_until(
        e2e, down ? H2_G99_COMPONENT_DOWN : H2_G99_COMPONENT_UP, &moved,
        H2_G99_E2E_NAV_MS, "main wheel rotation");
    if (rc != H2_PAL_OK) return rc;
  }
  h2_g99_e2e_fail(e2e, "main wheel never reached item %u", (unsigned)item);
  return H2_PAL_ERR_TIMEOUT;
}

h2_pal_result_t h2_g99_e2e_open_setting(h2_g99_e2e_t* e2e, uint8_t item) {
  h2_pal_result_t rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_SETTINGS);
  if (rc != H2_PAL_OK) return rc;
  const h2_g99_e2e_expect_t first =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, 0);
  rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &first,
                              H2_G99_E2E_NAV_MS, "Settings");
  for (uint8_t sel = 0u; rc == H2_PAL_OK && sel < item; ++sel) {
    const h2_g99_e2e_expect_t next =
        h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, sel + 1);
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &next,
                                H2_G99_E2E_NAV_MS, "settings wheel rotation");
  }
  return rc;
}

h2_pal_result_t h2_g99_e2e_wait_rest(h2_g99_e2e_t* e2e, uint32_t timeout_ms,
                                     const char* what) {
  const h2_g99_e2e_expect_t rest = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  return h2_g99_e2e_wait(e2e, &rest, NULL, NULL, timeout_ms, what, NULL);
}

/* After an input, waits briefly for Main to publish anything new. */
static void await_change(h2_g99_e2e_t* e2e, uint32_t sequence) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  h2_g99_observation_t o;
  while (h2_g99_e2e_now_ms(e2e) - start < 1000u) {
    if (h2_g99_e2e_observe(e2e, &o, NULL) && o.sequence != sequence) return;
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

h2_pal_result_t h2_g99_e2e_recover(h2_g99_e2e_t* e2e, uint32_t timeout_ms) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  while (h2_g99_e2e_now_ms(e2e) - start < timeout_ms) {
    if (!h2_g99_e2e_main_running(e2e)) {
      /* Main ended on its own; start a fresh one. */
      const h2_pal_result_t rc = h2_g99_e2e_main_restart(e2e);
      if (rc != H2_PAL_OK) return rc;
      continue;
    }
    h2_g99_observation_t o;
    if (!h2_g99_e2e_observe(e2e, &o, NULL) || (o.screen_on && !o.input_ready)) {
      h2_g99_e2e_sleep(e2e, 20u);
      continue;
    }
    if (h2_g99_e2e_at_rest(&o)) return H2_PAL_OK;
    h2_runtime_component_id_t key = H2_G99_COMPONENT_HOME;
    if (!o.screen_on) {
      key = H2_G99_COMPONENT_SCREEN;
    } else if (o.route == H2_G99_ROUTE_MAIN && !o.editing && !o.text_entry) {
      key = o.main_selection > H2_G99_MAIN_LIBRARY ? H2_G99_COMPONENT_UP
                                                   : H2_G99_COMPONENT_DOWN;
    }
    if (h2_g99_e2e_press(e2e, key) != H2_PAL_OK) return H2_PAL_ERR_IO;
    await_change(e2e, o.sequence);
  }
  fail_with_state(e2e, "timeout recovering", "rest", timeout_ms);
  return H2_PAL_ERR_TIMEOUT;
}
