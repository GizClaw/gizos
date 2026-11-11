/* GizPocket Loader on esp_mosaico: the public H2Loader over the Mosaico
 * Type-C CDC transport, with the GizPocket Loader screen on its callbacks.
 * It runs no G99 Main code. */

#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "h2_g99_loader_controller.h"
#include "h2_g99_screen_config.h"
#include "h2_mosaico_usb_console.h"

#include <stdio.h>

/* G99 orientation, shared with Main: see h2_g99_screen.h. */
static h2_g99_screen_t s_screen;
static h2_device_loader_t s_loader;

typedef struct loader_context {
  h2_g99_loader_controller_t* ui;
} loader_context_t;

/* The screen is best effort: the Loader serves without it. */
static void report(const char* phase, h2_pal_result_t rc) {
  if (rc != H2_PAL_OK)
    printf("H2_LOADER_UI_FAIL phase=%s code=%d\n", phase, rc);
}

static void show_installing(void* user) {
  loader_context_t* context = user;
  if (context->ui != NULL)
    report("installing", h2_g99_loader_controller_installing(context->ui));
}

static void install_progress(void* user, h2_loader_install_phase_t phase,
                             uint64_t completed, uint64_t total,
                             const char* detail) {
  (void)detail;
  loader_context_t* context = user;
  if (context->ui != NULL)
    report("progress", h2_g99_loader_controller_progress(context->ui, phase,
                                                         completed, total));
}

static void show_install_failed(void* user, int code) {
  loader_context_t* context = user;
  if (context->ui != NULL)
    report("failed", h2_g99_loader_controller_failed(context->ui, code));
}

static void show_launching(void* user) {
  loader_context_t* context = user;
  if (context->ui != NULL)
    report("launching", h2_g99_loader_controller_launching(context->ui));
}

static void loader_entry(void* user) {
  (void)user;
  h2_runtime_config_t config;
  h2_pal_result_t rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY target=esp status=runtime_config_fail code=%d\n",
           rc);
    return;
  }
  rc = h2_g99_screen_init(&s_screen, config.display, config.touch, config.mem,
                          H2_G99_SCREEN_ROTATION);
  if (rc == H2_PAL_OK) {
    config.display = h2_g99_screen_display(&s_screen);
    config.touch = h2_g99_screen_touch(&s_screen);
  } else {
    report("screen", rc);
    config.display = NULL;
    config.touch = NULL;
  }
  h2_runtime_t* runtime = NULL;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY target=esp status=runtime_init_fail code=%d\n", rc);
    return;
  }
  loader_context_t context = {0};
  rc = h2_g99_loader_controller_open(runtime, &context.ui);
  report("open", rc);
  if (rc != H2_PAL_OK) {
    while (context.ui != NULL) {
      report("close", h2_g99_loader_controller_close(&context.ui));
      if (context.ui != NULL) (void)h2_pal_time_sleep_ms(runtime->time, 20u);
    }
  }
  const h2_device_loader_config_t loader_config = {
      .board = "esp_mosaico",
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
      .user = &context,
      .install_progress = install_progress,
      .show_installing = show_installing,
      .show_install_failed = show_install_failed,
      .show_launching = show_launching,
  };
  rc = h2_device_loader_run(&s_loader, runtime, &loader_config, NULL, 0);
  if (rc != H2_PAL_OK)
    printf("H2_LOADER_READY target=esp status=service_fail code=%d\n", rc);
  while (context.ui != NULL) {
    report("close", h2_g99_loader_controller_close(&context.ui));
    if (context.ui != NULL) (void)h2_pal_time_sleep_ms(runtime->time, 20u);
  }
  while (runtime != NULL) {
    rc = h2_runtime_destroy(&runtime);
    if (runtime != NULL) (void)h2_pal_time_sleep_ms(config.time, 20u);
  }
  h2_g99_screen_deinit(&s_screen);
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
  if (h2_mosaico_usb_console_init() != H2_PAL_OK) return;
  /* Archive validation and flash writes nest deeply; keep them off IDF's
   * small app_main stack. */
  const h2_pal_result_t rc =
      h2_esp_board_start_entry_task("g99/loader", loader_entry, NULL);
  if (rc != H2_PAL_OK)
    printf("H2_BOARD_ENTRY_FAIL board=esp_mosaico image=g99-loader code=%d\n",
           rc);
}
