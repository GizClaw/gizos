#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_pal_storage_board_test_sdk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define H2_STORAGE_VERSION "console-test"
#define main firmware_main
#include "projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405/ap/ap_main.c"
#undef main

static h2_runtime_t actual;
static int suite_rc, stop_rc, restart_rc, confirm_rc;
static int active = 1, stops, starts, runs, replays, confirms, replay_after,
           settle, drain, repeats, finish_calls, finish_rc;
void rtos_delay_milliseconds(uint32_t ms) {
  if (ms == 60000u) {
    assert(!stops && !runs);
    ++settle;
  } else if (ms == 200u) {
    assert(!active && runs == 1 && replays == 1);
    ++drain;
  } else {
    assert(ms == 3000u && active && finished);
    ++repeats;
  }
}
int h2_bk_h2loader_stop_app_iostreamikcp(void) {
  assert(settle == 1 && !runs);
  ++stops;
  if (!stop_rc)
    active = 0;
  return stop_rc;
}
int h2_storage_device_run(h2_runtime_t *r, const char *path,
                          const char *version) {
  assert(r == &actual && !active && !strcmp(path, "/data/pal-storage") &&
         !strcmp(version, H2_STORAGE_VERSION));
  ++runs;
  return suite_rc;
}
void h2_storage_device_replay(h2_runtime_t *r) {
  assert(r == &actual && runs == 1);
  ++replays;
  if (active) {
    assert(starts == 1);
    ++replay_after;
  } else
    assert(!starts);
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *r, const char *name, uint32_t capabilities) {
  assert(r == &actual && !strcmp(name, "pal-storage") &&
         capabilities == H2_LOADER_CAPABILITY_UART && replays == 1 &&
         drain == 1);
  ++starts;
  if (!restart_rc)
    active = 1;
  return restart_rc;
}
int h2_bk_h2loader_confirm_current_app(h2_runtime_t *r) {
  assert(r == &actual && active && starts == 1 && suite_rc == 0);
  ++confirms;
  return confirm_rc;
}
int h2_application_host_should_stop(void *host) {
  (void)host;
  return repeats == 2;
}
h2_pal_result_t h2_pal_e2e_application_host_finish(h2_application_host_t *host,
                                                   h2_runtime_t *r,
                                                   h2_pal_result_t rc) {
  (void)host;
  assert(r == &actual);
  ++finish_calls;
  finish_rc = rc;
  return rc;
}
/* SDK entry boundaries are compiled but not invoked by the runner test. */
void bk_init(void) {}
int rtos_core0_create_psram_thread(beken_thread_t *t, int priority,
                                   const char *name, void (*entry)(void *),
                                   uint32_t stack, void *user) {
  (void)t;
  (void)priority;
  (void)name;
  (void)entry;
  (void)stack;
  (void)user;
  return 0;
}
int h2_bk_target_task_policy_install(void) { return 0; }
int h2_bk7258_board_start_entry_task(const char *name, void (*entry)(void *),
                                     void *user) {
  (void)name;
  (void)entry;
  (void)user;
  return 0;
}
h2_pal_result_t
h2_pal_e2e_application_host_run(const char *name, uint32_t capabilities,
                                const h2_application_host_config_t *config,
                                h2_application_host_result_t *result) {
  (void)name;
  (void)capabilities;
  (void)config;
  (void)result;
  assert(0);
  return H2_PAL_ERR_INVALID_STATE;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  int scenario = atoi(argv[1]);
  assert(scenario >= 0 && scenario <= 4);
  if (scenario == 1)
    suite_rc = H2_PAL_ERR_IO;
  if (scenario == 2)
    stop_rc = H2_PAL_ERR_TIMEOUT;
  if (scenario == 3)
    restart_rc = H2_PAL_ERR_IO;
  if (scenario == 4)
    confirm_rc = H2_PAL_ERR_IO;
  int rc = run(NULL, &actual, NULL);
  assert(finished && settle == 1 && stops == 1 && finish_calls == 1 &&
         rc == finish_rc);
  assert(rc == (scenario == 0   ? H2_PAL_OK
                : scenario == 2 ? H2_PAL_ERR_TIMEOUT
                                : H2_PAL_ERR_IO));
  if (scenario == 2)
    assert(!runs && !replays && !starts && !confirms);
  else if (scenario == 3)
    assert(runs == 1 && replays == 1 && starts == 1 && !confirms);
  else {
    assert(runs == 1 && replays == 3 && starts == 1 && replay_after == 2);
    assert(confirms == (scenario == 1 ? 0 : 1));
  }
  return 0;
}
