#include "bk_private/bk_init.h"
#include "device_runner.h"
#include "h2_bk7258_board.h"
#include "h2_device_service.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>
static h2_device_app_commands_t h2_management = {
    .config =
        {
            .active_name = "app",
            .hardware_capabilities =
                H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
            .h2loader_partition_id = 1u,
            .app_partition_id = 2u,
            .coredump_partition_id = 0x7a0000u,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
            .ble_start_attempts = 1u,
            .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
        },
};

static h2_runtime_t* runtime;
static volatile int finished;
static int commands_ready;
static uint16_t* scanout;
static h2_display_info_t display_info;
static const h2_pal_mem_api_t* bitmap_memory;

#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
/* The Board lends completed wire transfers while the serialized Display owner
 * runs the public cases. Keep that observation independent of its draw buffer.
 */
static struct {
  int next_y, error;
  size_t chunks, observed_chunks;
} capture_state;

static void capture(void* user, const h2_display_rect_t* rect,
                    const uint8_t* bytes, size_t length) {
  (void)user;
  if (!scanout || !rect || !bytes || rect->x != 0 || rect->y < 0 ||
      rect->width != display_info.width || rect->height <= 0 ||
      rect->y >= display_info.height ||
      rect->height > display_info.height - rect->y ||
      length != (size_t)rect->width * rect->height * 2u) {
    capture_state.error = H2_DISPLAY_ERR_IO;
    return;
  }
  if (rect->y == 0 && capture_state.next_y == display_info.height)
    capture_state.next_y = 0;
  if (rect->y != capture_state.next_y) {
    capture_state.error = H2_DISPLAY_ERR_IO;
    return;
  }
  for (int y = 0; y < rect->height; ++y)
    for (int x = 0; x < rect->width; ++x) {
      size_t offset = ((size_t)y * rect->width + x) * 2u;
      scanout[(size_t)(rect->y + y) * rect->width + x] =
          (uint16_t)((uint16_t)bytes[offset] << 8) | bytes[offset + 1u];
    }
  capture_state.next_y += rect->height;
  ++capture_state.chunks;
}
#endif

static int observe(void* user, const uint16_t* pixels, int width, int height,
                   uint32_t brightness, const char* id) {
  (void)user;
  if (width != display_info.width || height != display_info.height)
    return H2_DISPLAY_ERR_IO;
#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
  int rc = capture_state.error;
  if (!rc && (capture_state.next_y != height ||
              capture_state.chunks <= capture_state.observed_chunks ||
              H2_BK7258_BOARD_DISPLAY_BACKLIGHT_PERCENT() != brightness))
    rc = H2_DISPLAY_ERR_IO;
#else
  int rc = h2_bk7258_board_display_capture(scanout, (size_t)width * height);
#endif
  if (!rc && memcmp(pixels, scanout, (size_t)width * height * 2u))
    rc = H2_DISPLAY_ERR_IO;
#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
  printf(
      "H2_DISPLAY_OBSERVATION case=%s source=completed-transfer chunks=%zu "
      "brightness_command=%u optical_verified=0 rc=%d\n",
      id, capture_state.chunks - capture_state.observed_chunks,
      (unsigned)brightness, rc);
  capture_state.observed_chunks = capture_state.chunks;
#else
  printf(
      "H2_DISPLAY_OBSERVATION case=%s source=LCD-active-DMA "
      "refresh=observed optical_verified=0 brightness_command=%u rc=%d\n",
      id, (unsigned)brightness, rc);
#endif
  return rc;
}
static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000);
}
static void fail(const char* stage, int rc) {
  printf("H2_DISPLAY_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_ready) {
    rtos_delay_milliseconds(200u);
    (void)h2_pal_power_reboot(h2_device_app_commands_power(&h2_management), 0u);
  }
  hold();
}
static void watchdog(void* unused) {
  (void)unused;
  rtos_delay_milliseconds(600000);
  if (!finished) puts("H2_DISPLAY_WATCHDOG timeout=600s");
  hold();
}
static void run(void* unused) {
  (void)unused;
  rtos_delay_milliseconds(5000);
#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
  display_info = H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE(NULL, NULL);
#else
  /* Query the open provider, leaving Display closed for the public cases
   * even when get_info fails. */
  int prepare = h2_pal_display_open(runtime->display);
  if (!prepare) {
    prepare = h2_pal_display_get_info(runtime->display, &display_info);
    int closed = h2_pal_display_close(runtime->display);
    if (!prepare) prepare = closed;
  }
  if (prepare) fail("media-prepare", prepare);
#endif
  bitmap_memory = runtime->mem;
  if (display_info.width < 16 || display_info.height < 16 ||
      display_info.width > 4096 || display_info.height > 4096 ||
      display_info.native_format != H2_DISPLAY_PIXEL_RGB565)
    fail("capture-info", H2_DISPLAY_ERR_IO);
  scanout = h2_pal_mem_alloc(
      bitmap_memory, (size_t)display_info.width * display_info.height * 2u);
  if (!scanout) fail("capture-allocation", H2_DISPLAY_ERR_NO_MEMORY);
#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
  (void)H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE(capture, NULL);
#endif
  int rc = h2_display_device_run(runtime, H2_DISPLAY_VERSION, observe, NULL,
                                 bitmap_memory);
#ifdef H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE
  (void)H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE(NULL, NULL);
#endif
  h2_pal_mem_free(bitmap_memory, scanout);
  scanout = NULL;
  finished = 1;
  if (rc != H2_PAL_OK) {
    printf("H2_DISPLAY_QUALIFICATION_FAIL rc=%d confirm=not-attempted\n", rc);
    fail("qualification", rc);
  }
  int confirm = h2_device_app_confirm(&h2_management, runtime);
  if (confirm != H2_PAL_OK) fail("confirm", confirm);
  printf("H2_DISPLAY_READY rc=%d confirm=%d\n", rc, confirm);
  int visual = h2_display_device_demo(runtime);
  if (visual) fail("visual-demo", visual);
  visual = h2_display_device_show_pattern(runtime);
  if (visual) fail("stable-pattern", visual);
  printf("H2_DISPLAY_STABLE brightness=100 rc=%d optical_verified=0\n", visual);
  for (;;) {
    h2_display_device_replay(runtime);
    rtos_delay_milliseconds(3000);
  }
}
static void entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc) fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  rc = h2_device_app_commands_start_serial_named(
      &h2_management, runtime, "pal-display", H2_LOADER_CAPABILITY_UART);
  if (rc) fail("commands", rc);
  commands_ready = 1;
  beken_thread_t timer;
  if (rtos_core0_create_psram_thread(&timer, 7, "display_deadline", watchdog,
                                     4096, NULL) != kNoErr)
    fail("watchdog", H2_PAL_ERR_TASK);
  const h2_pal_task_options_t options = {.name = "pal-display/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-display", entry, NULL);
  if (rc) fail("entry", rc);
  return 0;
}
