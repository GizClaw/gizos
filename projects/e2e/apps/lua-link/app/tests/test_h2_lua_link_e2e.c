#include "h2_lua_link_e2e.h"
#include "h2_lua_link_fake_ble.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Both roles of the E2E App run against each other over the fake air, as the
 * two boards do, so the script itself is checked before any flash. */

typedef struct side {
  h2_runtime_t *runtime;
  const char *role;
  int hold;
  h2_pal_result_t rc;
} side_t;

static void *run_side(void *user) {
  side_t *side = user;
  side->rc = h2_lua_link_e2e_run(
      side->runtime, &(h2_lua_link_e2e_config_t){
                         .role = side->role,
                         .adv_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
                         .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
                         .hold = side->hold,
                     });
  return NULL;
}

static void run_pair(int hold) {
  fake_air_t air;
  side_t sides[2];
  pthread_t threads[2];
  fake_air_init(&air);
  for (int i = 0; i < 2; ++i) {
    sides[i] = (side_t){
        .runtime = fake_create_runtime(&air.devices[i].ble,
                                       &air.devices[i].events),
        .role = i == 0 ? "host" : "join",
        .hold = hold,
        .rc = H2_PAL_ERR_INVALID_STATE,
    };
    fake_set_baseline(&air.devices[i]);
    assert(pthread_create(&threads[i], NULL, run_side, &sides[i]) == 0);
  }
  if (hold) {
    /* Wait until both sides hold the link, then lose it without a BYE. */
    for (int i = 0; i < 20000 && !fake_snapshot(&air.devices[0]).connected;
         ++i) {
      (void)h2_pal_time_sleep_ms(sides[0].runtime->time, 1u);
    }
    assert(fake_snapshot(&air.devices[0]).connected);
    (void)h2_pal_time_sleep_ms(sides[0].runtime->time, 500u);
    fake_drop_link(&air);
  }
  for (int i = 0; i < 2; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(sides[i].rc == H2_PAL_OK);
    assert(fake_is_released(&air.devices[i]));
    h2_runtime_deinit(sides[i].runtime);
  }
}

/* Invalid inputs fail before anything is created; a Runtime without a usable
 * BLE Host fails at link enable and still tears the Lua Host down. */
static void test_rejected_runs(void) {
  fake_air_t air;
  fake_air_init(&air);
  h2_runtime_t *runtime =
      fake_create_runtime(&air.devices[0].ble, &air.devices[0].events);
  const h2_lua_link_e2e_config_t good = {
      .role = "host",
      .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
  };
  h2_lua_link_e2e_config_t bad = good;
  fake_set_baseline(&air.devices[0]);
  assert(h2_lua_link_e2e_run(NULL, &good) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_link_e2e_run(runtime, NULL) == H2_PAL_ERR_INVALID_ARG);
  bad.role = NULL;
  assert(h2_lua_link_e2e_run(runtime, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad.role = "spectator";
  assert(h2_lua_link_e2e_run(runtime, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = good;
  bad.adv_type = (h2_pal_ble_adv_type_t)7;
  assert(h2_lua_link_e2e_run(runtime, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = good;
  bad.scan_type = (h2_pal_ble_scan_type_t)7;
  assert(h2_lua_link_e2e_run(runtime, &bad) == H2_PAL_ERR_INVALID_ARG);
  assert(fake_is_released(&air.devices[0]));
  h2_runtime_deinit(runtime);

  runtime = fake_create_runtime(h2_pal_unsupported_ble_host_api(),
                                h2_pal_unsupported_system_event_api());
  assert(h2_lua_link_e2e_run(runtime, &good) == H2_PAL_ERR_UNSUPPORTED);
  h2_runtime_deinit(runtime);
}

int main(void) {
  test_rejected_runs();
  run_pair(0);
  run_pair(1);
  puts("lua link e2e tests passed");
  return 0;
}
