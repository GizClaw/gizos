#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_ipv6_device.h"

#include "bk_private/bk_init.h"
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t* runtime;
static h2_pal_ipv6_result_t result;
static int commands_ready;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}

static void fail(const char* stage, int rc) {
  printf("H2_PAL_IPV6_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_ready) {
    rtos_delay_milliseconds(200u);
    (void)h2_pal_power_reboot(h2_bk_h2loader_power_api(), 0u);
  }
  hold();
}

static void run(void* user) {
  (void)user;
  rtos_delay_milliseconds(5000u);
  int rc = h2_runtime_wifi_connect_saved(runtime, 20000u);
  if (rc != H2_PAL_OK) fail("wifi", rc);
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  if (rc != H2_PAL_OK) fail("clock", rc);
  for (;;) {
    h2_pal_net_addr_t local = {0};
    rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                         H2_PAL_NET_FAMILY_IPV6, &local);
    if (rc == H2_PAL_OK && (!h2_pal_ipv6_device_board_fixture() ||
                            (local.ip[0] == 0xfd && local.ip[1] == 0x53))) {
      printf("H2_PAL_IPV6_LINK local=");
      for (size_t i = 0u; i < sizeof(local.ip); i += 2u)
        printf("%s%02x%02x", i == 0u ? "" : ":", local.ip[i], local.ip[i + 1u]);
      printf(" peer=%s family=6\n", h2_pal_ipv6_device_host());
      fflush(stdout);
      break;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND &&
        rc != H2_PAL_ERR_UNAVAILABLE)
      fail("ipv6_address", rc);
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK) fail("clock", rc);
    if (now - start >= 20000u) fail("ipv6_address", H2_PAL_ERR_UNAVAILABLE);
    rtos_delay_milliseconds(100u);
  }
  /* Match the BK WebRTC launcher: conformance allocation tracks the peer's
   * PSRAM arena while the command service keeps the Board control heap. */
  h2_runtime_t test_runtime = *runtime;
  test_runtime.mem = h2_bk_platform_psram_allocator();
  rc =
      h2_pal_ipv6_device_run(&test_runtime, h2_bk_platform_dtls_api(), &result);
  int confirm = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && result.passed == H2_PAL_IPV6_CASES &&
      result.failed == 0u && result.blocked == 0u)
    confirm = h2_bk_h2loader_confirm_current_app(runtime);
  for (;;) {
    h2_pal_ipv6_device_report(runtime, &result);
    printf("H2_PAL_IPV6_READY board=%s rc=%d confirm=%d\n", runtime->board, rc,
           confirm);
    fflush(stdout);
    rtos_delay_milliseconds(5000u);
  }
}

static void entry(void* user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail("board", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime", rc);
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-ipv6",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
  if (rc != H2_PAL_OK) fail("commands", rc);
  commands_ready = 1;
  h2_pal_firmware_info_t image = {0};
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &image);
  if (rc != H2_PAL_OK) fail("firmware_info", rc);
  printf("H2_PAL_IPV6_BOOT board=%s version=%s\n", runtime->board,
         image.version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = h2_pal_ipv6_device_task_name};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK) fail("runner", rc);
  hold();
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-ipv6", entry, NULL);
  if (rc != H2_PAL_OK) fail("entry", rc);
  return 0;
}
