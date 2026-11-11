/* G99 product E2E on esp_mosaico: the step C suite against the production
 * Main on the board Runtime. Cases come from /data/e2e/select.txt, written by
 * --//projects/g99/apps/e2e:select at package time. The suite presses
 * HOME and SCREEN on the board keys and UP, DOWN and OK on virtual
 * push-edge Buttons; only Runtime Test Control drives them. */

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/h2_pal_display.h"
#include "os/h2_pal_fs.h"
#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "h2_g99_e2e.h"
#include "h2_g99_mosaico_input.h"
#include "h2_g99_screen_config.h"
#include "h2_mosaico_usb_console.h"

#include <stdio.h>
#include <string.h>

#define IMAGE_NAME "g99-e2e"
#define SELECT_PATH "/data/e2e/select.txt"

static h2_g99_mosaico_input_t s_input;
/* G99 orientation, shared with the Loader: see h2_g99_screen.h. */
static h2_g99_screen_t s_screen;
static h2_runtime_t* s_runtime;
static h2_device_app_commands_t s_management;

static void fail(const char* stage, int rc) {
  printf("H2_G99_E2E_BOOT stage=%s rc=%d\n", stage, rc);
}

/* Reads the selection; a missing file selects every case. */
static h2_pal_result_t read_select(char* out, size_t size) {
  out[0] = '\0';
  h2_pal_fs_file_t* file = NULL;
  int rc =
      h2_pal_fs_open(s_runtime->fs, SELECT_PATH, H2_PAL_FS_OPEN_READ, &file);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
  size_t length = 0u;
  rc = h2_pal_fs_read(s_runtime->fs, file, out, size - 1u, &length);
  (void)h2_pal_fs_close(s_runtime->fs, file);
  if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
  /* A selection longer than the suite accepts must not be cut silently. */
  if (length == size - 1u) return H2_PAL_ERR_NO_SPACE;
  out[length] = '\0';
  while (length > 0u && (out[length - 1u] == '\n' || out[length - 1u] == '\r' ||
                         out[length - 1u] == ' '))
    out[--length] = '\0';
  return H2_PAL_OK;
}

static void e2e_entry(void* user) {
  (void)user;
  static char select[H2_G99_E2E_SELECT_MAX + 2u];
  h2_pal_result_t rc = read_select(select, sizeof(select));
  h2_display_info_t info;
  memset(&info, 0, sizeof(info));
  if (rc == H2_PAL_OK) rc = h2_pal_display_open(s_runtime->display);
  if (rc == H2_PAL_OK) rc = h2_pal_display_get_info(s_runtime->display, &info);
  int exit_code = 2;
  int confirm = H2_PAL_ERR_INVALID_STATE;
  if (rc != H2_PAL_OK) {
    fail("setup", rc);
  } else {
    /* The board station is real; the Wi-Fi cases additionally need the
     * public test network in range, which the step E fixture provides.
     * The device holds the G99 frame budgets. */
    const h2_g99_e2e_target_t target = {
        .runtime = s_runtime,
        .board = "esp_mosaico",
        .target = "esp32s31",
        .display_width = (int32_t)info.width,
        .display_height = (int32_t)info.height,
        .capabilities =
            H2_G99_E2E_CAP_WIFI_TEST_NETWORK | H2_G99_E2E_CAP_FRAME_BUDGET,
        .select = select,
    };
    h2_g99_e2e_summary_t summary;
    rc = h2_g99_e2e_run(&target, &summary);
    if (rc == H2_PAL_OK) exit_code = h2_g99_e2e_exit_code(&summary);
    /* Keep a completed image installed even when cases failed; an image
     * that could not run the suite rolls back to the Loader. */
    if (exit_code != 2)
      confirm = h2_device_app_confirm(&s_management, s_runtime);
  }
  for (;;) {
    printf("H2_G99_E2E_DONE exit_code=%d confirm=%d version=%s\n", exit_code,
           confirm, esp_app_get_description()->version);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) {
    fail("task-policy", rc);
    return;
  }
  rc = h2_mosaico_usb_console_init();
  if (rc != H2_PAL_OK) {
    fail("usb", rc);
    return;
  }
  static h2_runtime_config_t config;
  static const h2_g99_mosaico_input_config_t input_config = {
      .home_periph_id = 101u,
      .screen_periph_id = 102u,
      .battery_periph_id = 301u,
      .virtual_first_id = 901u,
      .virtual_keys = true,
  };
  rc = h2_esp_board_runtime_config(&config);
  if (rc == H2_PAL_OK)
    rc = h2_g99_mosaico_input_init(&s_input, config.periph, &input_config);
  if (rc != H2_PAL_OK) {
    fail("board", rc);
    return;
  }
  config.periph = &s_input.periph;
  config.component_mapper = &s_input.mapper;
  rc = h2_g99_screen_init(&s_screen, config.display, config.touch, config.mem,
                          H2_G99_SCREEN_ROTATION);
  if (rc != H2_PAL_OK) {
    fail("screen", rc);
    return;
  }
  config.display = h2_g99_screen_display(&s_screen);
  config.touch = h2_g99_screen_touch(&s_screen);
  static const h2_device_app_commands_config_t commands = {
      .active_name = IMAGE_NAME,
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
  };
  rc = h2_device_app_commands_prepare_serial(&s_management, &config, &commands,
                                             NULL);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &s_runtime);
  if (rc == H2_PAL_OK) rc = h2_runtime_input_start(s_runtime, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("g99/e2e", e2e_entry, NULL);
  if (rc != H2_PAL_OK) fail("runtime", rc);
}
