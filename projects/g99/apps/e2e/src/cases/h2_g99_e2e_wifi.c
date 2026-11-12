#include "hal/h2_pal_wifi.h"
#include "h2_g99_e2e_internal.h"

#include <string.h>

#define SCAN_MS 15000u
#define CONNECT_MS 25000u
#define SAVED_BUSY_MS 5000u

static bool is_test_ssid(const char* ssid) {
  return strcmp(ssid, H2_G99_E2E_WIFI_SSID) == 0;
}

static bool connected_to_test(const h2_g99_observation_t* o, const void* arg) {
  (void)arg;
  return o->wifi_connected && is_test_ssid(o->wifi_ssid);
}

static bool scanned(const h2_g99_observation_t* o, const void* arg) {
  (void)arg;
  return o->wifi_status == H2_G99_WIFI_STATUS_IDLE &&
         o->wifi_network_count > 0u;
}

static bool focused_on(const h2_g99_observation_t* o, const void* arg) {
  return strcmp(o->wifi_focused_ssid, (const char*)arg) == 0;
}

/* Runtime Wi-Fi calls return BUSY while Main's worker owns them. */
static h2_pal_result_t saved_contains(h2_g99_e2e_t* e2e, bool* out) {
  h2_runtime_wifi_saved_network_t saved[H2_RUNTIME_WIFI_SAVED_MAX];
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    size_t count = 0u;
    const h2_pal_result_t rc = h2_runtime_wifi_saved_list(
        e2e->runtime, saved, H2_RUNTIME_WIFI_SAVED_MAX, &count);
    if (rc == H2_PAL_OK) {
      *out = false;
      for (size_t i = 0u; i < count; ++i)
        if (is_test_ssid(saved[i].config.ssid)) *out = true;
      memset(saved, 0, sizeof(saved));
      return H2_PAL_OK;
    }
    if (rc != H2_PAL_ERR_BUSY ||
        h2_g99_e2e_now_ms(e2e) - start >= SAVED_BUSY_MS) {
      h2_g99_e2e_errorf(e2e, rc, "saved Wi-Fi list");
      return rc;
    }
    h2_g99_e2e_sleep(e2e, 50u);
  }
}

static h2_pal_result_t saved_remove(h2_g99_e2e_t* e2e) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  for (;;) {
    const h2_pal_result_t rc = h2_runtime_wifi_saved_remove(
        e2e->runtime, H2_G99_E2E_WIFI_SSID, strlen(H2_G99_E2E_WIFI_SSID));
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
    if (rc != H2_PAL_ERR_BUSY ||
        h2_g99_e2e_now_ms(e2e) - start >= SAVED_BUSY_MS) {
      h2_g99_e2e_errorf(e2e, rc, "remove saved Wi-Fi");
      return rc;
    }
    h2_g99_e2e_sleep(e2e, 50u);
  }
}

/* The fixture: whether the test network was saved and connected before the
 * case, so the case can put both back. */
typedef struct journal {
  bool was_saved;
  bool was_connected;
} journal_t;

/* Fixture state comes from the station itself: Main's observation of the link
 * lags it by up to one worker poll. */
static bool station_on_test(h2_g99_e2e_t* e2e) {
  h2_pal_wifi_sta_status_t status;
  memset(&status, 0, sizeof(status));
  return h2_pal_wifi_sta_get_status(e2e->runtime->wifi_sta, &status) ==
             H2_PAL_OK &&
         h2_pal_wifi_sta_status_has_ip(&status) && is_test_ssid(status.ssid);
}

static h2_pal_result_t journal_open(h2_g99_e2e_t* e2e, journal_t* j) {
  j->was_connected = station_on_test(e2e);
  return saved_contains(e2e, &j->was_saved);
}

static bool link_down(const h2_g99_observation_t* o, const void* arg) {
  (void)arg;
  return !o->wifi_connected;
}

/* Drops the station link and waits until Main at rest shows it down. */
static h2_pal_result_t drop_link(h2_g99_e2e_t* e2e) {
  const int disconnect = h2_pal_wifi_sta_disconnect(e2e->runtime->wifi_sta);
  if (disconnect != H2_PAL_OK && disconnect != H2_PAL_ERR_INVALID_STATE) {
    h2_g99_e2e_errorf(e2e, (h2_pal_result_t)disconnect, "station disconnect");
    return (h2_pal_result_t)disconnect;
  }
  const h2_g99_e2e_expect_t rest = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  return h2_g99_e2e_wait(e2e, &rest, link_down, NULL, H2_G99_E2E_AFTER_CASE_MS,
                         "link down", NULL);
}

static h2_pal_result_t journal_restore(h2_g99_e2e_t* e2e, const journal_t* j) {
  h2_pal_result_t rc = H2_PAL_OK;
  if (!j->was_saved) rc = saved_remove(e2e);
  /* The next case starts only once Main shows the link down again. */
  if (!j->was_connected) {
    const h2_pal_result_t dropped = drop_link(e2e);
    if (rc == H2_PAL_OK) rc = dropped;
  }
  return rc;
}

/* From rest: Settings > Wi-Fi, pick the test network, type its password and
 * wait for the connection, then return to rest. */
static h2_pal_result_t connect_through_settings(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_WIFI);
  h2_g99_e2e_expect_t list =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_WIFI);
  list.editing = 1;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_OK, &list,
                                H2_G99_E2E_NAV_MS, "Wi-Fi list");
  h2_g99_observation_t o;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &list, scanned, NULL, SCAN_MS, "Wi-Fi scan", &o);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_stays(e2e, &list, 300u, "Wi-Fi controls revealed");
  /* Rows wrap, so at most one full turn finds the network. */
  for (unsigned i = 0u; rc == H2_PAL_OK && i <= o.wifi_network_count; ++i) {
    if (h2_g99_e2e_observe(e2e, &o, NULL) &&
        focused_on(&o, H2_G99_E2E_WIFI_SSID))
      break;
    if (i == o.wifi_network_count) {
      h2_g99_e2e_fail(e2e, "%s not in the scan list of %u",
                      H2_G99_E2E_WIFI_SSID, (unsigned)o.wifi_network_count);
      rc = H2_PAL_ERR_NOT_FOUND;
      break;
    }
    const uint32_t sequence = o.sequence;
    rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_DOWN);
    for (int wait = 0; rc == H2_PAL_OK && wait < 100; ++wait) {
      if (h2_g99_e2e_observe(e2e, &o, NULL) && o.sequence != sequence) break;
      h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
    }
  }
  h2_g99_e2e_expect_t keyboard = list;
  keyboard.text_entry = 1;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_tap(e2e, (o.info_rect.x1 + o.info_rect.x2) / 2,
                        o.height * 17 / 20);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &keyboard, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "Connect button opens password entry", NULL);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_type(e2e, H2_G99_E2E_WIFI_PASSWORD);
  /* Text and keys share Runtime's ordered event queue. */
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
  /* A successful connect closes the list. */
  const h2_g99_e2e_expect_t page =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_WIFI);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &page, connected_to_test, NULL, CONNECT_MS,
                         "Wi-Fi connected", NULL);
  const h2_g99_e2e_expect_t back = h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &back,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static h2_pal_result_t finish(h2_g99_e2e_t* e2e, const journal_t* j,
                              h2_pal_result_t rc) {
  /* Restore from rest so Main's worker is idle. */
  (void)h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  const h2_pal_result_t restore = journal_restore(e2e, j);
  return rc != H2_PAL_OK ? rc : restore;
}

static h2_pal_result_t connect_test_network(h2_g99_e2e_t* e2e) {
  journal_t j;
  h2_pal_result_t rc = journal_open(e2e, &j);
  if (rc != H2_PAL_OK) return rc;
  /* Start disconnected so the connection is the one made here; the case
   * leaves the link up again, as it found it. */
  if (j.was_connected) rc = drop_link(e2e);
  if (rc == H2_PAL_OK) rc = connect_through_settings(e2e);
  return finish(e2e, &j, rc);
}

/* Main reconnects to the saved test network on its own after a restart. */
static h2_pal_result_t reconnect_after_restart(h2_g99_e2e_t* e2e) {
  journal_t j;
  h2_pal_result_t rc = journal_open(e2e, &j);
  if (rc != H2_PAL_OK) return rc;
  /* The restart must find the network both saved and last used. */
  if (!(j.was_saved && j.was_connected)) rc = connect_through_settings(e2e);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_stop(e2e);
  /* Fixture boundary: drop the link while no Main runs, so only the next
   * Main's saved-network reconnect can bring it back. */
  if (rc == H2_PAL_OK) {
    const int disconnect = h2_pal_wifi_sta_disconnect(e2e->runtime->wifi_sta);
    if (disconnect != H2_PAL_OK) {
      h2_g99_e2e_errorf(e2e, (h2_pal_result_t)disconnect, "fixture disconnect");
      rc = (h2_pal_result_t)disconnect;
    }
  }
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_start(e2e);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait_rest(e2e, H2_G99_E2E_START_MS, "Main restart");
  /* Reconnect happens in the background; the menu stays usable. */
  const h2_g99_e2e_expect_t rest = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &rest, connected_to_test, NULL, CONNECT_MS,
                         "reconnect after restart", NULL);
  return finish(e2e, &j, rc);
}

static bool list_settled(const h2_g99_observation_t* observation,
                         const void* argument) {
  (void)argument;
  return observation->edit_index == 1u && observation->wheel_drag == 0.0f &&
         !observation->text_entry;
}

static bool left_following(const h2_g99_observation_t* observation,
                           const void* argument) {
  (void)argument;
  return observation->wheel_drag > 0.6f && observation->edit_index == 1u &&
         !observation->text_entry;
}

static h2_pal_result_t wait_list_idle(h2_g99_e2e_t* e2e) {
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  uint64_t changed_at = start;
  uint32_t previous = 0u;
  for (;;) {
    h2_g99_e2e_frames_t frames;
    h2_g99_e2e_frames_get(e2e, &frames);
    const uint64_t now = h2_g99_e2e_now_ms(e2e);
    if (frames.frames != previous) {
      changed_at = now;
      previous = frames.frames;
    }
    if (now - changed_at >= 120u && frames.frames >= 2u) {
      h2_g99_e2e_metric(e2e, "phase=wifi-snap frames=%u",
                        (unsigned)frames.frames);
      return H2_PAL_OK;
    }
    if (now - start >= H2_G99_E2E_NAV_MS) {
      h2_g99_e2e_fail(e2e, "Wi-Fi snap did not draw and settle");
      return H2_PAL_ERR_TIMEOUT;
    }
    h2_g99_e2e_sleep(e2e, H2_G99_E2E_POLL_MS);
  }
}

static h2_pal_result_t dual_wheel_gestures(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_open_setting(e2e, H2_G99_SETTING_WIFI);
  h2_g99_e2e_expect_t wifi =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_WIFI);
  h2_g99_observation_t observation = {0};
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, scanned, NULL, SCAN_MS, "Wi-Fi networks",
                         &observation);
  if (rc == H2_PAL_OK && observation.wifi_network_count < 2u) {
    h2_g99_e2e_fail(e2e, "Wi-Fi gesture fixture needs at least two networks");
    return H2_PAL_ERR_NOT_FOUND;
  }
  const int32_t right =
      (observation.info_rect.x1 + observation.info_rect.x2) / 2;
  const int32_t row_height = observation.height / 10;
  const int32_t start = observation.height / 2;
  h2_g99_e2e_frames_reset(e2e);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_touch_down(e2e, right, start);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_move(e2e, observation.width / 10,
                               start - row_height * 7 / 10);
  wifi.editing = 1;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "right-side selection", NULL);
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_stays(e2e, &wifi, 300u, "right list follows across divider");
  h2_g99_e2e_frames_t following;
  h2_g99_e2e_frames_get(e2e, &following);
  if (rc == H2_PAL_OK && following.frames < 3u) {
    h2_g99_e2e_fail(e2e, "right Wi-Fi list did not draw while held");
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_g99_e2e_frames_reset(e2e);
  h2_pal_result_t lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, list_settled, NULL, H2_G99_E2E_NAV_MS,
                         "right list snaps to one network", &observation);
  if (rc == H2_PAL_OK) rc = wait_list_idle(e2e);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_tap(e2e, right, observation.height / 2);
  if (rc == H2_PAL_OK)
    rc =
        h2_g99_e2e_stays(e2e, &wifi, 150u, "tapping a network only selects it");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_drag(e2e, right, observation.height * 17 / 20,
                         observation.width / 10, observation.height / 4, 250u,
                         true, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_stays(e2e, &wifi, 150u,
                          "dragging the Connect button never connects");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_down(e2e, observation.width / 10,
                               observation.height * 3 / 4);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_touch_move(e2e, right, observation.height / 2);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &wifi, left_following, NULL, H2_G99_E2E_NAV_MS,
                         "left wheel follows across divider", NULL);
  lifted = h2_g99_e2e_touch_up(e2e);
  if (rc == H2_PAL_OK) rc = lifted;
  const h2_g99_e2e_expect_t volume =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, H2_G99_SETTING_VOLUME);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait(e2e, &volume, NULL, NULL, H2_G99_E2E_NAV_MS,
                         "left wheel leaves Wi-Fi for Volume", NULL);
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &settings,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"dual-wheel-gestures", H2_G99_E2E_CAP_WIFI_TEST_NETWORK,
     dual_wheel_gestures},
    {"connect-test-network", H2_G99_E2E_CAP_WIFI_TEST_NETWORK,
     connect_test_network},
    {"reconnect-after-restart", H2_G99_E2E_CAP_WIFI_TEST_NETWORK,
     reconnect_after_restart},
};

const h2_g99_e2e_group_t h2_g99_e2e_wifi_group = {
    "wifi", kCases, sizeof(kCases) / sizeof(kCases[0])};
