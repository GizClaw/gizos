/* One worker per boot; existing workload registries retain their assertions. */
#include "h2_runtime.h"
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_loader_usb.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#if H2_MOSAICO_SUITE_STORAGE
#include "device_runner.h"
#elif H2_MOSAICO_SUITE_DISPLAY
#include "device_runner.h"
#include "h2_mosaico_display_capture.h"
#include "bsp/display.h"
#elif H2_MOSAICO_SUITE_AUDIO
#include "h2_pal_audio_e2e.h"
#include "esp_system.h"
#elif H2_MOSAICO_SUITE_LIBCO
#include "h2_libco_smoke.h"
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#include "esp_private/hw_stack_guard.h"
#endif
#endif
static h2_runtime_t* runtime;
#if H2_MOSAICO_SUITE_LIBCO
static const h2_pal_log_api_t* libco_board_log;
/* Keep the maintained libco launcher's result channel visible under the
 * board's normal log filtering policy. */
static int libco_result_write(void* user, h2_pal_log_level_t level,
                              const char* scope, const char* message) {
  (void)user;
  if (scope && message && strcmp(scope, "libco-smoke") == 0)
    return printf("%s\n", message) < 0 || fflush(stdout) != 0 ? H2_PAL_ERR_IO
                                                              : H2_PAL_OK;
  return h2_pal_log_write(libco_board_log, level, scope, message);
}
static const h2_pal_log_vtable_t libco_result_vtable = {.write =
                                                            libco_result_write};
static const h2_pal_log_api_t libco_result_log = {.vtable =
                                                      &libco_result_vtable};
#endif
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
static esp_timer_handle_t watchdog;
static void hold(void) {
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("H2_MOSAICO_SETUP stage=%s rc=%d qualified=0\n", stage, rc);
  hold();
}
static void deadline(void* unused) {
  (void)unused;
  puts("H2_MOSAICO_WATCHDOG timeout_s=180 qualified=0 recovery=manual");
  fflush(stdout);
}
#endif
#if H2_MOSAICO_SUITE_CAMERA
static h2_pal_camera_frame_t loan;
static int camera_run(void) {
  for (unsigned cycle = 0; cycle < 10; ++cycle) {
    int rc = h2_pal_camera_start(runtime->camera);
    printf("H2_MOSAICO_CAMERA_START cycle=%u rc=%d\n", cycle, rc);
    if (rc) {
      int cleanup = H2_PAL_ERR_IO;
      for (unsigned retry = 0; retry < 3 && cleanup; ++retry)
        cleanup = h2_pal_camera_stop(runtime->camera);
      printf("H2_MOSAICO_CAMERA_START_CLEANUP rc=%d\n", cleanup);
      return rc;
    }
    int first = 0;
    for (unsigned n = 0; n < 30; ++n) {
      int64_t before = esp_timer_get_time();
      rc = h2_pal_camera_acquire(runtime->camera, &loan);
      printf(
          "H2_MOSAICO_CAMERA cycle=%u frame=%u acquire=%d elapsed_us=%" PRId64
          "\n",
          cycle, n, rc, esp_timer_get_time() - before);
      if (rc) {
        first = rc;
        break;
      }
      int valid = loan.data && loan.width == 1280 && loan.height == 720 &&
                  loan.format == H2_PAL_CAMERA_FORMAT_UYVY &&
                  loan.stride >= 2560 &&
                  loan.size >= (size_t)loan.stride * loan.height;
      uint32_t hash = 2166136261u;
      if (valid)
        for (size_t i = 0; i < loan.size; ++i)
          hash = (hash ^ loan.data[i]) * 16777619u;
      printf("H2_MOSAICO_FRAME valid=%d bytes=%zu hash=%08" PRIx32
             " optical_verified=0\n",
             valid, loan.size, hash);
      int busy = h2_pal_camera_stop(runtime->camera);
      for (unsigned retry = 0; retry < 3; ++retry) {
        rc = h2_pal_camera_release(runtime->camera, &loan);
        if (rc)
          printf("H2_MOSAICO_CAMERA_RELEASE_RETRY attempt=%u rc=%d\n",
                 retry + 1, rc);
        if (!rc) break;
      }
      printf("H2_MOSAICO_CAMERA_RELEASE cycle=%u frame=%u busy=%d release=%d\n",
             cycle, n, busy, rc);
      if (rc) return rc; /* Static descriptor and provider remain alive. */
      memset(&loan, 0, sizeof(loan));
      if (!valid || busy != H2_PAL_ERR_BUSY) {
        first = H2_PAL_ERR_IO;
        break;
      }
    }
    for (unsigned retry = 0; retry < 3; ++retry) {
      rc = h2_pal_camera_stop(runtime->camera);
      if (rc)
        printf("H2_MOSAICO_CAMERA_STOP_RETRY attempt=%u rc=%d\n", retry + 1,
               rc);
      if (!rc) break;
    }
    printf("H2_MOSAICO_CAMERA_STOP cycle=%u first=%d stop=%d\n", cycle, first,
           rc);
    if (first || rc) return first ? first : rc;
  }
  return 0;
}
#elif H2_MOSAICO_SUITE_DISPLAY
static uint16_t* transferred;
static size_t chunks;
static int capture_error;
static void capture(void* user, const h2_display_rect_t* r,
                    const uint16_t* pixels) {
  (void)user;
  if (!r || !pixels || r->x < 0 || r->y < 0 || r->width <= 0 ||
      r->height <= 0 || r->x > BSP_LCD_H_RES - r->width ||
      r->y > BSP_LCD_V_RES - r->height) {
    capture_error = 1;
    return;
  }
  const uint8_t* bytes = (const uint8_t*)pixels;
  for (int y = 0; y < r->height; ++y)
    for (int x = 0; x < r->width; ++x) {
      size_t i = ((size_t)y * r->width + x) * 2;
      transferred[(size_t)(r->y + y) * BSP_LCD_H_RES + r->x + x] =
          (uint16_t)((uint16_t)bytes[i] << 8) | bytes[i + 1];
    }
  ++chunks;
}
static int observe(void* user, const uint16_t* expected, int w, int h,
                   uint32_t brightness, const char* id) {
  (void)user;
  int rc =
      capture_error || !chunks || w != BSP_LCD_H_RES || h != BSP_LCD_V_RES
          ? H2_PAL_ERR_IO
          : (memcmp(expected, transferred, (size_t)w * h * 2) ? H2_PAL_ERR_IO
                                                              : 0);
  printf(
      "H2_MOSAICO_DISPLAY case=%s source=completed-SPI-DMA chunks=%zu brightness=%" PRIu32
      " optical_verified=0 rc=%d\n",
      id, chunks, brightness, rc);
  return rc;
}
#elif H2_MOSAICO_SUITE_AUDIO
static void audio_report(void* user, const h2_pal_audio_e2e_case_result_t* r) {
  (void)user;
  printf("H2_MOSAICO_AUDIO case=%s passed=%d blocked=%d detail=%d line=%u\n",
         r->id, r->passed, r->blocked, r->detail, r->line);
  fflush(stdout);
}
#elif H2_MOSAICO_SUITE_PERIPHERALS
static int peripherals_run(void) {
  int first = 0;
  unsigned buttons[2] = {0};
  for (unsigned n = 0; n < 100; ++n) {
    for (unsigned i = 0; i < 2; ++i) {
      h2_pal_single_button_reading_t b;
      int rc = h2_pal_button_read_single_button(runtime->button, 101 + i, &b);
      if (rc && !first) first = rc;
      if (!rc && !h2_pal_button_state_is_valid(b.state)) {
        rc = H2_PAL_ERR_IO;
        if (!first) first = rc;
      }
      if (!rc && h2_pal_button_state_is_valid(b.state))
        buttons[i] |= 1u << b.state;
      printf("H2_MOSAICO_BUTTON id=%u sample=%u rc=%d state=%d\n", 101 + i, n,
             rc, rc ? -1 : (int)b.state);
    }
    for (unsigned i = 0; i < 3; ++i) {
      h2_pal_imu_reading_t v;
      int rc = h2_pal_imu_read(runtime->imu, 201 + i, &v);
      uint32_t required =
          i ? H2_PAL_IMU_HAS_MAG : H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO;
      if (!rc && (v.id != 201 + i || (v.flags & required) != required))
        rc = H2_PAL_ERR_IO;
      if (rc && !first) first = rc;
      printf(
          "H2_MOSAICO_SENSOR id=%u sample=%u rc=%d flags=%u accel=%ld,%ld,%ld gyro=%ld,%ld,%ld mag=%ld,%ld,%ld\n",
          201 + i, n, rc, rc ? 0 : (unsigned)v.flags,
          rc ? 0L : (long)v.accel_mg.x, rc ? 0L : (long)v.accel_mg.y,
          rc ? 0L : (long)v.accel_mg.z, rc ? 0L : (long)v.gyro_mdps.x,
          rc ? 0L : (long)v.gyro_mdps.y, rc ? 0L : (long)v.gyro_mdps.z,
          rc ? 0L : (long)v.mag_mgauss.x, rc ? 0L : (long)v.mag_mgauss.y,
          rc ? 0L : (long)v.mag_mgauss.z);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  h2_pal_battery_reading_t battery;
  int rc = h2_pal_input_read_battery(runtime->input, 301, &battery);
  const uint32_t battery_flags = H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                                 H2_PAL_BATTERY_HAS_CURRENT_MA |
                                 H2_PAL_BATTERY_HAS_PERCENT_X100;
  if (!rc && (battery.id != 301 || battery.percent_x100 > 10000 ||
              (battery.flags & battery_flags) != battery_flags))
    rc = H2_PAL_ERR_IO;
  if (rc && !first) first = rc;
  printf(
      "H2_MOSAICO_BATTERY rc=%d voltage_mv=%ld current_ma=%ld percent_x100=%u physical_verified=0\n",
      rc, rc ? 0L : (long)battery.voltage_mv,
      rc ? 0L : (long)battery.current_ma,
      rc ? 0 : (unsigned)battery.percent_x100);
  rc = h2_pal_pwm_switch_set_duty(runtime->pwm_switch, 401, 3000);
  vTaskDelay(pdMS_TO_TICKS(250));
  int off = H2_PAL_ERR_IO;
  for (unsigned retry = 0; retry < 3 && off; ++retry)
    off = h2_pal_pwm_switch_set_duty(runtime->pwm_switch, 401, 0);
  if ((rc || off) && !first) first = rc ? rc : off;
  printf(
      "H2_MOSAICO_MOTOR pulse_ms=250 duty=3000 rc=%d off=%d physical_verified=0\n",
      rc, off);
  rc = h2_pal_touch_open(runtime->touch);
  unsigned edges = 0;
  if (!rc) {
    h2_pal_touch_info_t info = {0};
    int info_rc = h2_pal_touch_get_info(runtime->touch, &info);
    if ((info_rc || !info.width || !info.height) && !first)
      first = info_rc ? info_rc : H2_PAL_ERR_IO;
    int64_t end = esp_timer_get_time() + 30000000;
    while (esp_timer_get_time() < end) {
      h2_pal_touch_event_t e;
      int poll = h2_pal_touch_poll_event(runtime->touch, &e);
      if (!poll) {
        printf("H2_MOSAICO_TOUCH kind=%d x=%ld y=%ld\n", (int)e.kind, (long)e.x,
               (long)e.y);
        if (!info_rc && e.x >= 0 && e.y >= 0 && (uint32_t)e.x < info.width &&
            (uint32_t)e.y < info.height && e.kind >= H2_PAL_TOUCH_EVENT_DOWN &&
            e.kind <= H2_PAL_TOUCH_EVENT_UP)
          edges |= 1u << e.kind;
        else if (!first)
          first = H2_PAL_ERR_IO;
      } else if (poll != H2_PAL_ERR_WOULD_BLOCK) {
        if (!first) first = poll;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    rc = h2_pal_touch_close(runtime->touch);
  }
  if (rc && !first) first = rc;
  unsigned expected = (1u << H2_PAL_TOUCH_EVENT_DOWN) |
                      (1u << H2_PAL_TOUCH_EVENT_MOVE) |
                      (1u << H2_PAL_TOUCH_EVENT_UP);
  unsigned button_expected = (1u << H2_PAL_BUTTON_STATE_PRESSED) |
                             (1u << H2_PAL_BUTTON_STATE_RELEASED);
  int complete = edges == expected && buttons[0] == button_expected &&
                 buttons[1] == button_expected;
  printf(
      "H2_MOSAICO_INTERACTION touch_edges=%u buttons=%u,%u complete=%d physical_verified=0\n",
      edges, buttons[0], buttons[1], complete);
  return first ? first : complete ? 0 : H2_PAL_ERR_UNAVAILABLE;
}
#endif
#ifndef H2_MOSAICO_LAUNCHER_HOST_TEST
static void run(void* unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(5000));
  int rc = H2_PAL_ERR_UNSUPPORTED;
#if H2_MOSAICO_SUITE_STORAGE
  rc = h2_storage_device_run(runtime, "/data/mosaico-e2e",
                             esp_app_get_description()->version);
#elif H2_MOSAICO_SUITE_LIBCO
  BaseType_t root_core = xTaskGetCoreID(NULL);
  printf("H2_MOSAICO_LIBCO_ROOT task=%s task_core=%d current_core=%d\n",
         pcTaskGetName(NULL), (int)root_core, (int)xPortGetCoreID());
  if (root_core == tskNO_AFFINITY || root_core != xPortGetCoreID())
    fail("libco-root-core", H2_PAL_ERR_INVALID_STATE);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
  uint32_t stack_low = 0, stack_high = 0;
  portMUX_TYPE guard_lock = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&guard_lock);
  esp_hw_stack_guard_get_bounds((uint32_t)root_core, &stack_low, &stack_high);
  portEXIT_CRITICAL(&guard_lock);
  printf("H2_MOSAICO_LIBCO_GUARD low=0x%08" PRIx32 " high=0x%08" PRIx32
         " root_local=%p enabled=1\n",
         stack_low, stack_high, (void*)&root_core);
  fflush(stdout);
#endif
  rc = h2_libco_smoke_run(runtime, NULL);
  if (!rc &&
      (root_core != xTaskGetCoreID(NULL) || root_core != xPortGetCoreID()))
    rc = H2_PAL_ERR_INVALID_STATE;
#elif H2_MOSAICO_SUITE_CAMERA
  rc = camera_run();
#elif H2_MOSAICO_SUITE_PERIPHERALS
  rc = peripherals_run();
#elif H2_MOSAICO_SUITE_AUDIO
  static h2_pal_audio_e2e_result_t result;
  const h2_pal_audio_e2e_config_t config = {.audio = runtime->audio,
                                            .time = runtime->time,
                                            .stability_ms = 30000,
                                            .report = audio_report};
  rc = h2_pal_audio_e2e_run(&config, &result);
  printf(
      "H2_MOSAICO_AUDIO_SUMMARY passed=%u failed=%u blocked=%u mic_frames=%" PRIu32
      " mic_peak=%" PRIu32 " acoustic_verified=0\n",
      result.passed, result.failed, result.blocked, result.mic_frames,
      result.mic_peak);
#elif H2_MOSAICO_SUITE_DISPLAY
  transferred =
      h2_pal_mem_alloc(runtime->mem, (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
  if (!transferred) fail("capture-memory", H2_PAL_ERR_NO_MEMORY);
  memset(transferred, 0, (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
  h2_mosaico_display_set_transfer_capture(capture, NULL);
  static h2_pal_display_e2e_result_t result;
  const h2_pal_display_e2e_config_t config = {
      .supported_formats = 1u << H2_DISPLAY_PIXEL_RGB565,
      .clips_rectangles = 0,
      .observe = observe};
  rc = h2_pal_display_e2e_run(runtime, &config, &result);
  h2_mosaico_display_set_transfer_capture(NULL, NULL);
  h2_pal_mem_free(runtime->mem, transferred);
  transferred = NULL;
  if (!rc) rc = h2_display_device_demo(runtime);
  if (!rc) rc = h2_display_device_show_pattern(runtime);
  printf("H2_MOSAICO_DISPLAY_STABLE rc=%d optical_verified=0\n", rc);
#endif
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
#if H2_MOSAICO_SUITE_AUDIO
  if (rc) {
    /* A failed close may leave a borrowed track attached. Preserve the ledger,
     * then reset the unconfirmed trial instead of leaving playback active. */
    for (unsigned i = 0; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i)
      audio_report(NULL, &result.cases[i]);
    printf(
        "H2_MOSAICO_E2E_DONE rc=%d confirm=not-attempted software_pass=0 physical_qualified=0 recovery=reset\n",
        rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
#endif
  int confirm =
      rc ? H2_PAL_ERR_INVALID_STATE : h2_esp_h2loader_app_confirm(runtime);
  for (;;) {
    printf(
        "H2_MOSAICO_E2E_DONE rc=%d confirm=%d software_pass=%d physical_qualified=0\n",
        rc, confirm, !rc);
#if H2_MOSAICO_SUITE_STORAGE
    h2_storage_device_replay(runtime);
#elif H2_MOSAICO_SUITE_AUDIO
    for (unsigned i = 0; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i)
      audio_report(NULL, &result.cases[i]);
#elif H2_MOSAICO_SUITE_DISPLAY
    h2_pal_display_e2e_print(&result, "esp_mosaico", rc, 0);
#endif
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("task-policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc) fail("usb-console", rc);
  rc = h2_mosaico_loader_usb_init();
  if (rc) fail("usb-transport", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
#if H2_MOSAICO_SUITE_LIBCO
  libco_board_log = config.log;
  config.log = &libco_result_log;
#endif
  config.system_event = h2_pal_unsupported_system_event_api();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "mosaico-e2e",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "mosaico-e2e"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 180000000) != ESP_OK)
    fail("watchdog", H2_PAL_ERR_IO);
  printf("H2_MOSAICO_E2E_BOOT version=%s board=esp_mosaico\n",
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {.name = "mosaico/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("worker", rc);
}

#endif
