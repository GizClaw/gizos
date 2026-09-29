#include "device_runner.h"
#include <stdio.h>
static h2_pal_display_e2e_result_t result;
static const char *image_version;
static int run_rc, complete;
int h2_display_device_run(h2_runtime_t *runtime, const char *version,
                          h2_pal_display_e2e_observe_fn observe, void *user,
                          const h2_pal_mem_api_t *working_mem) {
  if (image_version || !runtime || !version)
    return H2_DISPLAY_ERR_INVALID_STATE;
  image_version = version;
  printf("H2_DISPLAY_BOOT version=%s\n", version);
  const h2_pal_display_e2e_config_t test = {.working_mem = working_mem,
                                            .supported_formats = 7,
                                            .clips_rectangles = 1,
                                            .observe = observe,
                                            .user = user};
  run_rc = h2_pal_display_e2e_run(runtime, &test, &result);
  complete = 1;
  puts("H2_DISPLAY_PHASE run");
  h2_pal_display_e2e_print(&result, "board-driver", run_rc, 0);
  return run_rc;
}
void h2_display_device_replay(h2_runtime_t *runtime) {
  if (!complete)
    return;
  puts("H2_DISPLAY_PHASE replay");
  printf("H2_DISPLAY_IDENTITY version=%s evidence=driver-transfer "
         "optical_verified=0\n",
         image_version);
  h2_pal_display_e2e_print(&result, "board-driver", run_rc, 0);
  h2_pal_time_sleep_ms(runtime->time, 500);
}

int h2_display_device_demo(h2_runtime_t *runtime) {
  h2_display_info_t info = {0};
  int rc = h2_pal_display_open(runtime->display);
  if (!rc)
    rc = h2_pal_display_get_info(runtime->display, &info);
  if (!rc && (info.width <= 0 || info.height <= 0 || info.width > 4096 ||
              info.height > 4096))
    rc = H2_DISPLAY_ERR_IO;
  uint16_t *row =
      rc ? NULL : h2_pal_mem_alloc(runtime->mem, (size_t)info.width * 2u);
  if (!rc && !row)
    rc = H2_DISPLAY_ERR_NO_MEMORY;
  const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
  for (int y = 0; !rc && y < info.height; ++y) {
    for (int x = 0; x < info.width; ++x)
      row[x] = x == 0 || y == 0 || x == info.width - 1 || y == info.height - 1
                   ? 0xffff
                   : colors[(x >= info.width / 2) + 2 * (y >= info.height / 2)];
    const h2_display_rect_t rect = {0, y, info.width, 1};
    rc = h2_pal_display_draw_bitmap(runtime->display, &rect, row,
                                    (size_t)info.width * 2u,
                                    H2_DISPLAY_PIXEL_RGB565);
  }
  h2_pal_mem_free(runtime->mem, row);
  if (!rc)
    rc = h2_pal_display_present(runtime->display);
  const uint32_t levels[] = {100, 50, 0, 100};
  for (unsigned i = 0; !rc && i < 4; ++i) {
    rc = h2_pal_display_set_brightness_percent(runtime->display, levels[i]);
    if (!rc)
      rc = h2_pal_display_present(runtime->display);
    printf("H2_DISPLAY_VISUAL brightness=%u rc=%d optical_verified=0\n",
           (unsigned)levels[i], rc);
    h2_pal_time_sleep_ms(runtime->time, 2500);
  }
  int close = h2_pal_display_close(runtime->display);
  return rc ? rc : close;
}
