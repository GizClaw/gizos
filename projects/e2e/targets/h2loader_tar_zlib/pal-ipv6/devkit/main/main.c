#if defined(ESP_PLATFORM)
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "sdkconfig.h"
#endif
#include "h2_pal_ipv6_device.h"
#include "h2_atomic.h"
#include "h2_atomic_static.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include "h2/pal/hal/h2_pal_usb_jtag_io_stream.h"
#include <stdio.h>
#include <string.h>

#ifndef H2_PAL_IPV6_BOARD
#define H2_PAL_IPV6_BOARD H2_ESP_BOARD_NAME
#endif

static struct {
  const h2_pal_sync_api_t *sync;
  h2_pal_mutex_t *mutex;
  const h2_pal_uart_io_stream_api_t *uart;
  const h2_pal_usb_jtag_io_stream_api_t *usb;
} console;
H2_ATOMIC_DEFINE_STATIC(int, report_error, H2_PAL_OK);

static int remember_report_error(int rc) {
  if (rc != H2_PAL_OK) {
    int expected = H2_PAL_OK;
    (void)h2_atomic_int_compare_exchange(&report_error, &expected, rc,
        H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST);
  }
  return h2_atomic_int_load(&report_error, H2_ATOMIC_SEQ_CST);
}

/* Result output borrows H2Loader's configured console. A private frame and one
 * serialized PAL write/drain prevent concurrent callbacks from mixing records.
 * A partial write is terminal; a later replay cannot repair this first ledger. */
static int write_report(void *user, h2_pal_log_level_t level,
                        const char *scope, const char *message) {
  (void)user;
  (void)level;
  (void)scope;
  int rc = remember_report_error(H2_PAL_OK);
  if (rc != H2_PAL_OK)
    return rc;
  char record[514];
  size_t length = 0u;
  if (!message)
    return remember_report_error(H2_PAL_ERR_INVALID_ARG);
  while (length < sizeof(record) - 2u && message[length]) {
    if (message[length] == '\r' || message[length] == '\n')
      return remember_report_error(H2_PAL_ERR_FORMAT);
    record[length] = message[length];
    ++length;
  }
  if (!length || length == sizeof(record) - 2u)
    return remember_report_error(H2_PAL_ERR_TRUNCATED);
  record[length++] = '\r';
  record[length++] = '\n';
  rc = h2_pal_mutex_lock(console.sync, console.mutex);
  if (rc != H2_PAL_OK)
    return remember_report_error(rc);
  rc = remember_report_error(H2_PAL_OK);
  if (rc == H2_PAL_OK) {
    size_t written = 0u;
    rc = console.uart
        ? h2_pal_uart_io_stream_write(console.uart, record, length, &written, 5000u)
        : h2_pal_usb_jtag_io_stream_write(console.usb, record, length, &written, 5000u);
    if (rc == H2_PAL_OK && written != length)
      rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK)
      rc = console.uart ? h2_pal_uart_io_stream_flush(console.uart)
                        : h2_pal_usb_jtag_io_stream_flush(console.usb);
    (void)remember_report_error(rc);
  }
  int unlocked = h2_pal_mutex_unlock(console.sync, console.mutex);
  return remember_report_error(unlocked);
}
static const h2_pal_log_vtable_t report_vtable = {.write = write_report};
static const h2_pal_log_api_t report_log = {.vtable = &report_vtable};

static int emit_report(const char *line, size_t capacity, int length) {
  if (length < 0 || (size_t)length >= capacity)
    return remember_report_error(H2_PAL_ERR_TRUNCATED);
  return h2_pal_log_write(&report_log, H2_PAL_LOG_INFO, "pal-ipv6", line);
}

static int confirm_after_report(h2_runtime_t *runtime,
    const h2_pal_ipv6_result_t *result, int suite_rc,
    int (*confirm)(h2_runtime_t *)) {
  if (!runtime || !result || !confirm)
    return H2_PAL_ERR_INVALID_ARG;
  int delivered = h2_pal_ipv6_device_report_with_log(runtime, &report_log, result);
  if (suite_rc != H2_PAL_OK)
    return suite_rc;
  if (delivered != H2_PAL_OK)
    return delivered;
  if (result->cleanup_error || result->retained_sockets || result->retained_resolvers ||
      result->retained_allocations || result->retained_tasks || result->retained_cleanup ||
      result->retained_runtime)
    return result->cleanup_error ? result->cleanup_error : H2_PAL_ERR_INVALID_STATE;
  int rc = remember_report_error(H2_PAL_OK);
  return rc == H2_PAL_OK ? confirm(runtime) : rc;
}
#if defined(ESP_PLATFORM)
static h2_runtime_t *runtime;
static h2_pal_ipv6_result_t result;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_IPV6_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc;
  if (h2_pal_ipv6_device_board_fixture()) {
    /* Temporary association: the connect operation never saves credentials. */
    h2_pal_wifi_sta_config_t wifi = {.ssid = "h2ipv6-amoled",
                                     .ssid_len = 13,
                                     .password = "palipv6e2e",
                                     .password_len = 10,
                                     .channel = 6};
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &wifi, 20000u);
    memset(&wifi, 0, sizeof(wifi));
  } else {
    rc = h2_runtime_wifi_connect_saved(runtime, 20000u);
  }
  if (rc != H2_PAL_OK)
    fail("saved_wifi", rc);
  uint64_t start = 0u;
  if (h2_pal_time_get_monotonic_ms(runtime->time, &start) != H2_PAL_OK)
    fail("ipv6_clock", H2_PAL_ERR_IO);
  for (;;) {
    h2_pal_net_addr_t local;
    rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                         H2_PAL_NET_FAMILY_IPV6, &local);
    if (rc == H2_PAL_OK && (!h2_pal_ipv6_device_board_fixture() ||
                            (local.ip[0] == 0xfd && local.ip[1] == 0x53))) {
      char line[200];
      int length = snprintf(line, sizeof(line),
               "H2_PAL_IPV6_LINK "
               "local=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%"
               "02x%02x:%02x%02x peer=%s family=6",
               local.ip[0], local.ip[1], local.ip[2], local.ip[3], local.ip[4],
               local.ip[5], local.ip[6], local.ip[7], local.ip[8], local.ip[9],
               local.ip[10], local.ip[11], local.ip[12], local.ip[13],
               local.ip[14], local.ip[15], h2_pal_ipv6_device_host());
      rc = emit_report(line, sizeof(line), length);
      if (rc != H2_PAL_OK)
        fail("link_delivery", rc);
      break;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND &&
        rc != H2_PAL_ERR_UNAVAILABLE)
      fail("ipv6_address", rc);
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &now) != H2_PAL_OK ||
        now - start >= 20000u)
      fail("ipv6_address", H2_PAL_ERR_UNAVAILABLE);
    vTaskDelay(pdMS_TO_TICKS(100u));
  }
  rc = h2_pal_ipv6_device_run_with_report_log(
      runtime, h2_esp_platform_dtls_api(), &report_log, &result);
  if (result.passed + result.failed + result.blocked != H2_PAL_IPV6_CASES)
    fail("ipv6_network_or_fixture", rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc);
  const int confirm = confirm_after_report(runtime, &result, rc,
                                            h2_esp_h2loader_app_confirm);
  if (confirm != H2_PAL_OK)
    fail("ledger_or_confirm", confirm);
  for (;;) {
    char line[160];
    int length = snprintf(line, sizeof(line),
        "H2_PAL_IPV6_READY board=%s rc=%d confirm=%d", H2_PAL_IPV6_BOARD, rc, confirm);
    int delivered = emit_report(line, sizeof(line), length);
    if (delivered != H2_PAL_OK)
      fail("ready_delivery", delivered);
    vTaskDelay(pdMS_TO_TICKS(5000u));
    delivered = h2_pal_ipv6_device_report_with_log(runtime, &report_log, &result);
    if (delivered != H2_PAL_OK)
      fail("ledger_delivery", delivered);
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-ipv6", 1u, 3u);
  if (rc != H2_PAL_OK)
    fail("command_prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  console.sync = runtime->sync;
  const h2_pal_mutex_config_t lock_config = {
      .name = "pal-ipv6/records", .allocator = runtime->mem};
  rc = h2_pal_mutex_create(console.sync, &lock_config, &console.mutex);
  if (rc != H2_PAL_OK)
    fail("report_mutex", rc);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  console.usb = h2_esp_platform_usb_jtag_io_stream_api();
#elif CONFIG_ESP_CONSOLE_UART
  console.uart = h2_esp_platform_uart_io_stream_api();
#else
  fail("report_console", H2_PAL_ERR_UNSUPPORTED);
#endif
  char line[160];
  int length = snprintf(line, sizeof(line), "H2_PAL_IPV6_BOOT board=%s version=%s",
      H2_PAL_IPV6_BOARD, esp_app_get_description()->version);
  rc = emit_report(line, sizeof(line), length);
  if (rc != H2_PAL_OK)
    fail("boot_delivery", rc);
  const h2_pal_task_options_t options = {.name = h2_pal_ipv6_device_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
}
#endif
