#include "startup_test_sdk.h"

#include <assert.h>
#include <stddef.h>

extern int h2_bk_cp_main(void);
extern void bk_module_init(void);

static beken_thread_function_t user_entry;
static int events[4];
static size_t event_count;
static int policy_result, frequency_result;
static unsigned fixed_boot_calls;

enum { POLICY = 1, FREQUENCY, AP_BOOT };

static void record(int event) {
  assert(event_count < sizeof(events) / sizeof(events[0]));
  events[event_count++] = event;
}

int h2_bk_target_task_policy_install(void) {
  record(POLICY);
  return policy_result;
}

int bk_pm_module_vote_cpu_freq(int module, int frequency) {
  assert(module == PM_DEV_ID_DEFAULT && frequency == PM_CPU_FRQ_480M);
  record(FREQUENCY);
  return frequency_result;
}

int bk_pm_module_vote_boot_cp1_ctrl(int module, int state) {
  assert(module == PM_BOOT_CP1_MODULE_NAME_APP);
  assert(state == PM_POWER_MODULE_STATE_ON);
  record(AP_BOOT);
  return BK_OK;
}

void h2_bk_cp_try_fixed_app(void) { ++fixed_boot_calls; }

void rtos_set_user_app_entry(beken_thread_function_t entry) {
  assert(entry != NULL);
  user_entry = entry;
}

int bk_init(void) {
  assert(user_entry != NULL);
  bk_module_init();
  user_entry();
  return 73;
}

static void reset(void) {
  event_count = 0u;
  policy_result = frequency_result = 0;
  fixed_boot_calls = 0u;
  user_entry = NULL;
}

int main(void) {
  /* The early SDK callback must not access Flash/IPC before main. */
  reset();
  bk_module_init();
  assert(fixed_boot_calls == 0u);

  /* The configured floor must be established before AP is powered on. */
  assert(h2_bk_cp_main() == 73);
  assert(fixed_boot_calls == 1u);
  assert(events[0] == POLICY);
#if CONFIG_H2_AP_CPU_FREQ_480M
  assert(event_count == 3u);
  assert(events[1] == FREQUENCY && events[2] == AP_BOOT);
#else
  assert(event_count == 2u && events[1] == AP_BOOT);
#endif

  /* Neither failure may boot AP with a partially applied startup policy. */
  reset();
  policy_result = -1;
  assert(h2_bk_cp_main() == 73);
  assert(event_count == 1u && events[0] == POLICY);
#if CONFIG_H2_AP_CPU_FREQ_480M
  reset();
  frequency_result = -1;
  assert(h2_bk_cp_main() == 73);
  assert(event_count == 2u);
  assert(events[0] == POLICY && events[1] == FREQUENCY);
#endif
  return 0;
}
