#include "h2_lua_link_e2e.h"
#include "h2_lua_link_fake_ble.h"

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

int main(void) {
  run_pair(0);
  run_pair(1);
  puts("lua link e2e tests passed");
  return 0;
}
