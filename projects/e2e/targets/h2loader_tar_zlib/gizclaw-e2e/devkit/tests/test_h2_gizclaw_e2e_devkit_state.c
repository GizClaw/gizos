#include "h2_gizclaw_e2e_devkit_state.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct wifi_fixture {
  int settings_rc;
  int connect_rc;
  h2_pal_wifi_sta_config_t saved;
  unsigned connect_count;
  h2_pal_wifi_sta_config_t connected;
  uint32_t connect_timeout_ms;
} wifi_fixture_t;

static int wifi_get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
  (void)user;
  *out_status = (h2_pal_wifi_sta_status_t){
      .state = H2_PAL_WIFI_STA_STATE_DISCONNECTED,
  };
  return H2_PAL_OK;
}

static int wifi_connect(void *user, const h2_pal_wifi_sta_config_t *config,
                        uint32_t timeout_ms) {
  wifi_fixture_t *fixture = user;
  ++fixture->connect_count;
  fixture->connected = *config;
  fixture->connect_timeout_ms = timeout_ms;
  return fixture->connect_rc;
}

static int settings_get(void *user, h2_pal_wifi_sta_config_t *out_config) {
  wifi_fixture_t *fixture = user;
  if (fixture->settings_rc == H2_PAL_OK) {
    *out_config = fixture->saved;
  }
  return fixture->settings_rc;
}

static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
    .get_status = wifi_get_status,
    .connect = wifi_connect,
};

static const h2_pal_wifi_settings_vtable_t settings_vtable = {
    .get_saved_sta_config = settings_get,
};

static void test_wifi_step_uses_only_valid_saved_configuration(void) {
  wifi_fixture_t fixture = {.settings_rc = H2_PAL_OK};
  memcpy(fixture.saved.ssid, "saved-network", 13u);
  fixture.saved.ssid_len = 13u;
  memcpy(fixture.saved.password, "saved-password", 14u);
  fixture.saved.password_len = 14u;
  const h2_pal_wifi_sta_api_t wifi = {
      .user = &fixture,
      .vtable = &wifi_vtable,
  };
  const h2_pal_wifi_settings_api_t settings = {
      .user = &fixture,
      .vtable = &settings_vtable,
  };
  h2_gizclaw_e2e_devkit_wifi_result_t result;

  assert(h2_gizclaw_e2e_devkit_wifi_step(
             &wifi, &settings, 4321u, &result) == H2_PAL_OK);
  assert(result.outcome == H2_GIZCLAW_E2E_DEVKIT_WIFI_CONNECTED);
  assert(result.rc == H2_PAL_OK);
  assert(fixture.connect_count == 1u);
  assert(fixture.connect_timeout_ms == 4321u);
  assert(fixture.connected.ssid_len == fixture.saved.ssid_len);
  assert(memcmp(fixture.connected.ssid, fixture.saved.ssid,
                fixture.saved.ssid_len) == 0);
  assert(fixture.connected.password_len == fixture.saved.password_len);
  assert(memcmp(fixture.connected.password, fixture.saved.password,
                fixture.saved.password_len) == 0);

  fixture.connect_rc = H2_PAL_ERR_TIMEOUT;
  fixture.connect_count = 0u;
  assert(h2_gizclaw_e2e_devkit_wifi_step(
             &wifi, &settings, 4321u, &result) == H2_PAL_OK);
  assert(result.outcome == H2_GIZCLAW_E2E_DEVKIT_WIFI_RETRY);
  assert(result.rc == H2_PAL_ERR_TIMEOUT);
  assert(fixture.connect_count == 1u);
  assert(fixture.connect_timeout_ms == 4321u);
  fixture.connect_rc = H2_PAL_OK;

  const int unavailable[] = {
      H2_PAL_ERR_NOT_FOUND,
      H2_PAL_ERR_IO,
  };
  for (size_t i = 0u; i < sizeof(unavailable) / sizeof(unavailable[0]); ++i) {
    fixture.settings_rc = unavailable[i];
    fixture.connect_count = 0u;
    assert(h2_gizclaw_e2e_devkit_wifi_step(
               &wifi, &settings, 4321u, &result) == H2_PAL_OK);
    assert(result.outcome == H2_GIZCLAW_E2E_DEVKIT_WIFI_NO_SAVED_CONFIG);
    assert(result.rc == unavailable[i]);
    assert(fixture.connect_count == 0u);
  }

  fixture.settings_rc = H2_PAL_OK;
  fixture.saved = (h2_pal_wifi_sta_config_t){0};
  assert(h2_gizclaw_e2e_devkit_wifi_step(
             &wifi, &settings, 4321u, &result) == H2_PAL_OK);
  assert(result.outcome == H2_GIZCLAW_E2E_DEVKIT_WIFI_NO_SAVED_CONFIG);
  assert(result.rc == H2_PAL_ERR_INVALID_ARG);
  assert(fixture.connect_count == 0u);
}

static void test_runner_starts_only_after_network_and_clock(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);

  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, false,
                                                         false));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, false));
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, false, true));
  assert(!h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, false));
  assert(state.runner_started);
  assert(state.wifi_has_ip);
  assert(state.clock_ready);
}

static void test_summary_replay_is_not_early(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  h2_gizclaw_e2e_devkit_state_complete(&state, 100u, 10000u);

  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 10099u,
                                                          10000u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 10100u,
                                                         10000u));
  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 20099u,
                                                          10000u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 20100u,
                                                         10000u));
}

static void test_summary_replay_handles_monotonic_wrap(void) {
  h2_gizclaw_e2e_devkit_state_t state;
  h2_gizclaw_e2e_devkit_state_init(&state);
  assert(h2_gizclaw_e2e_devkit_state_set_prerequisites(&state, true, true));
  h2_gizclaw_e2e_devkit_state_complete(&state, UINT64_MAX - 5u, 10u);

  assert(!h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 3u, 10u));
  assert(h2_gizclaw_e2e_devkit_state_take_summary_replay(&state, 4u, 10u));
}

int main(void) {
  test_wifi_step_uses_only_valid_saved_configuration();
  test_runner_starts_only_after_network_and_clock();
  test_summary_replay_is_not_early();
  test_summary_replay_handles_monotonic_wrap();
  return 0;
}
