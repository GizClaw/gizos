#include "launcher.h"

#include <bk_private/bk_init.h>
#include <inttypes.h>
#include <os/os.h>
#include <soc/reg_base.h>
#include <stdio.h>
#include <string.h>

#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_disk_e2e.h"

/* Bounded output does not depend on the SDK's asynchronous logging worker. */
static int emit(void* user, const char* phase,
                const h2_pal_disk_e2e_result_t* result) {
  (void)user;
  char line[512];
  int len;
  if (strcmp(phase, "case") == 0) {
    static const char* const statuses[] = {"NOT_RUN", "PASS", "FAIL",
                                           "BLOCKED"};
    const unsigned index = (unsigned)result->current_case;
    if (index >= H2_PAL_DISK_E2E_CASE_COUNT ||
        result->cases[index].status > H2_PAL_DISK_E2E_BLOCKED)
      return H2_PAL_ERR_INVALID_ARG;
    len = snprintf(
        line, sizeof(line), "\r\nH2_PAL_DISK_CASE id=%s status=%s rc=%d\r\n",
        h2_pal_disk_e2e_case_name(result->current_case),
        statuses[result->cases[index].status], result->cases[index].rc);
  } else {
    len = snprintf(
        line, sizeof(line),
        "\r\nH2_PAL_DISK_E2E contract=%u phase=%s rc=%d cleanup_rc=%d output_rc=%d"
        " passed=%" PRIu32 " failed=%" PRIu32 " blocked=%" PRIu32
        " writer_rounds=%" PRIu32 " reader_rounds=%" PRIu32
        " writer_stage=%" PRIu32 " reader_stage=%" PRIu32 " cores=%" PRIu32
        ",%" PRIu32 " cores_verified=%d workers_joined=%d\r\n",
        H2_PAL_DISK_E2E_CONTRACT_VERSION, phase, result->rc, result->cleanup_rc,
        result->output_rc, result->passed, result->failed, result->blocked,
        result->rounds[0], result->rounds[1], result->stage[0],
        result->stage[1], result->core[0], result->core[1],
        result->cores_verified, result->workers_joined);
  }
  if (len < 0 || (size_t)len >= sizeof(line)) return H2_PAL_ERR_TRUNCATED;
  volatile uint32_t* uart = (volatile uint32_t*)SOC_UART1_REG_BASE;
  for (int i = 0; i < len; ++i) {
    unsigned attempts = 0;
    for (; attempts < 200000u; ++attempts) {
      const uint32_t status = uart[6];
      if (!(status & (1u << 16u)) && (status & (1u << 20u))) {
        uart[7] = (uint8_t)line[i];
        break;
      }
    }
    if (attempts == 200000u) return H2_PAL_ERR_WRITE;
  }
  return 0;
}

static uint32_t current_core(void* user) {
  (void)user;
  return (uint32_t)rtos_get_core_id();
}

static void controller(void* argument) {
  (void)argument;
  h2_pal_disk_e2e_result_t result = {0};
  result.rc = h2_bk_pal_disk_management_start();
  int output = emit(NULL, "management_ready", &result);
  if (!result.rc && !output) {
    /* Leave a managed status/reboot window before stressing IO. */
    rtos_delay_milliseconds(10000);
    const h2_pal_disk_e2e_config_t config = {
        .disk = h2_bk_pal_disk_disk(),
        .task = h2_bk_platform_task_api(),
        .time = h2_bk_platform_time_api(),
        .mem = h2_bk_platform_sram_allocator(),
        .partition_id = H2_BK_PAL_DISK_PARTITION,
        .current_core = current_core,
        /* AP-local task policy cores 0/1 are SDK chip core IDs 1/2. */
        .expected_core = {1, 2},
        .emit = emit,
    };
    h2_pal_disk_e2e_t* app = NULL;
    result.rc = h2_pal_disk_e2e_create(&config, &app);
    if (!result.rc) result.rc = h2_pal_disk_e2e_run(app, &result);
    (void)emit(NULL, "run_returned", &result);
    int release;
    do {
      release = h2_pal_disk_e2e_destroy(&app);
      if (release == H2_PAL_ERR_BUSY) rtos_delay_milliseconds(1000);
    } while (release == H2_PAL_ERR_BUSY);
    /* A teardown error retains app and its dependencies on this controller. */
    result.cleanup_rc = release;
    (void)emit(NULL, "release", &result);
  }
  for (;;) rtos_delay_milliseconds(1000);
}

int main(void) {
  if (h2_bk_target_task_policy_install() != 0) return -1;
  bk_init();
  const h2_pal_disk_e2e_result_t result = {0};
  if (emit(NULL, "boot", &result)) return -1;
  beken_thread_t thread;
  return rtos_core0_create_thread(&thread, 6, "flash_control", controller,
                                  16384, NULL);
}
