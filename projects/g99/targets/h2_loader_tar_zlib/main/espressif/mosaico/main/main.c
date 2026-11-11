/* GizPocket Main on esp_mosaico. AI is HOME (back), BOOT is SCREEN (screen
 * off/on), POWER stays with the hardware and touch provides rotate and OK.
 * Settings and Wi-Fi use the board's pref and station. The image confirms
 * itself once Main is ready on its menu. Main draws only when something
 * moves; each run of frames prints one H2_G99_PERF line of frame timing,
 * at least every 2 s while it lasts. */

#include "esp_app_desc.h"
#include "h2_esp_board.h"
#include "h2_esp_platform_core.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "h2_g99.h"
#include "h2_g99_mosaico_input.h"
#include "h2_g99_screen_config.h"
#include "h2_mosaico_usb_console.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_NAME "g99-main"
#define PERF_WINDOW_US 2000000u
/* A pause this long between two frames ends a run. */
#define PERF_PAUSE_US 1000000u

static h2_g99_mosaico_input_t s_input;
/* G99 orientation, shared with the Loader: see h2_g99_screen.h. */
static h2_g99_screen_t s_screen;
static h2_runtime_t* s_runtime;
static h2_device_app_commands_t s_management;
static bool s_confirmed;

/* Frame timing of the current run, written by the Main task only. */
static struct {
  uint64_t window_us;
  uint32_t frames;
  uint32_t max_interval_us;
  uint64_t work_us;
  uint32_t max_work_us;
  uint32_t max_draw_us;
  uint32_t max_present_us;
} s_perf;

static void fail(const char* stage, int rc) {
  printf("H2_G99_MAIN_BOOT stage=%s rc=%d\n", stage, rc);
}

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void print_perf(void) {
  if (s_perf.frames > 1u)
    printf(
        "H2_G99_PERF window_ms=%u frames=%u interval_max_us=%u "
        "work_avg_us=%u work_max_us=%u draw_max_us=%u present_max_us=%u\n",
        (unsigned)(s_perf.window_us / 1000u), (unsigned)s_perf.frames,
        (unsigned)s_perf.max_interval_us,
        (unsigned)(s_perf.work_us / s_perf.frames),
        (unsigned)s_perf.max_work_us, (unsigned)s_perf.max_draw_us,
        (unsigned)s_perf.max_present_us);
  memset(&s_perf, 0, sizeof(s_perf));
}

static void on_frame(void* user, const h2_g99_frame_stats_t* f) {
  (void)user;
  if (s_perf.frames > 0u && f->interval_us >= PERF_PAUSE_US) print_perf();
  ++s_perf.frames;
  if (s_perf.frames > 1u) {
    s_perf.window_us += f->interval_us;
    s_perf.max_interval_us = max_u32(s_perf.max_interval_us, f->interval_us);
  }
  s_perf.work_us += f->work_us;
  s_perf.max_work_us = max_u32(s_perf.max_work_us, f->work_us);
  s_perf.max_draw_us = max_u32(s_perf.max_draw_us, f->draw_us);
  s_perf.max_present_us = max_u32(s_perf.max_present_us, f->present_us);
  if (s_perf.window_us >= PERF_WINDOW_US) print_perf();
}

/* Confirm on the first settled menu: the image booted, drew and takes
 * input. Runs once, on the Main task. */
static void observe(void* user, const h2_g99_observation_t* o) {
  (void)user;
  static bool observed;
  static h2_g99_route_t route;
  static uint8_t main_selection, sub_selection;
  if (o->lifecycle != H2_G99_LIFECYCLE_RUNNING || !o->input_ready) return;
  if (!observed || route != o->route || main_selection != o->main_selection ||
      sub_selection != o->sub_selection) {
    observed = true;
    route = o->route;
    main_selection = o->main_selection;
    sub_selection = o->sub_selection;
    printf("H2_G99_STATE route=%d main=%u sub=%u screen=%d\n", o->route,
           (unsigned)o->main_selection, (unsigned)o->sub_selection,
           o->screen_on ? 1 : 0);
  }
  if (!s_confirmed && o->route == H2_G99_ROUTE_MAIN) {
    s_confirmed = true;
    const int rc = h2_device_app_confirm(&s_management, s_runtime);
    printf("H2_G99_MAIN_CONFIRM rc=%d\n", rc);
  }
}

static void main_entry(void* user) {
  (void)user;
  const char* version = esp_app_get_description()->version;
  const h2_g99_config_t config = {
      .main_version = version,
      .board_name = "esp_mosaico",
      .observe = observe,
      .frame = on_frame,
      .render_mem = h2_esp_platform_internal_allocator(),
  };
  printf("H2_G99_MAIN_BOOT version=%s board=esp_mosaico\n", version);
  const h2_pal_result_t rc = h2_g99_run(s_runtime, &config);
  printf("H2_G99_MAIN_EXIT rc=%d\n", rc);
  for (;;) (void)h2_pal_time_sleep_ms(s_runtime->time, 1000u);
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
  /* Return-to-Loader commands over USB serial while Main runs. */
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
    rc = h2_esp_board_start_entry_task("g99/main", main_entry, NULL);
  if (rc != H2_PAL_OK) fail("runtime", rc);
}
