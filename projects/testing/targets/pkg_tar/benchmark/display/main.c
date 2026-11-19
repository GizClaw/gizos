#include "display_bench.h"
#include "h2_web_board_config.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten/threading.h>
#include <stdio.h>

#define PREVIEW_HOLD_MS 1000u

/* clang-format off */
EM_JS(void, read_stop_request,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => Module.h2WebStopRequested ? 1 : 0);
});
/* clang-format on */

static int run_preview(h2_web_platform_t* platform) {
  const h2_pal_display_api_t* display = h2_web_platform_display_api(platform);
  const h2_pal_mem_api_t* memory = h2_web_platform_mem_api();
  const h2_display_rect_t rect = {0, 0, H2_WEB_BOARD_WIDTH,
                                  H2_WEB_BOARD_HEIGHT};
  const size_t frame_pixels = (size_t)rect.width * rect.height;
  uint16_t* pixels =
      h2_pal_mem_alloc(memory, frame_pixels * 2u * sizeof(*pixels));
  if (pixels == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_display_bench_fill_patterns(pixels, rect.width, rect.height);

  int rc = h2_pal_display_open(display);
  bool first_frame = true;
  size_t offset = 0;
  while (rc == H2_PAL_OK && !h2_web_main_call(read_stop_request, NULL).i32) {
    rc = h2_display_bench_draw_frame(display, &rect, pixels + offset);
    if (rc != H2_PAL_OK) break;
    if (first_frame) {
      printf(
          "H2_WEB_APP name=display_bench_preview preview=RGB565 "
          "size=%dx%d hold_ms=%u\n",
          rect.width, rect.height, PREVIEW_HOLD_MS);
      first_frame = false;
    }
    offset = frame_pixels - offset;
    // The worker sleeps between draws so the browser can paint every pattern.
    h2_web_worker_sleep(PREVIEW_HOLD_MS);
  }
  h2_pal_mem_free(memory, pixels);
  return rc;
}

int main(void) {
  const h2_web_platform_config_t config = {
      .display_width = H2_WEB_BOARD_WIDTH,
      .display_height = H2_WEB_BOARD_HEIGHT};
  h2_web_platform_t* platform = emscripten_is_main_runtime_thread()
                                    ? NULL
                                    : h2_web_platform_create(&config);
  const int rc =
      platform == NULL ? H2_PAL_ERR_UNAVAILABLE : run_preview(platform);
  const int destroy =
      platform == NULL ? H2_PAL_OK : h2_web_platform_destroy(platform);
  const bool ok = rc == H2_PAL_OK && destroy == H2_PAL_OK;
  printf("H2_WEB_APP name=display_bench_preview result=%s rc=%d destroy=%d\n",
         ok ? "PASS" : "FAIL", rc, destroy);
  return ok ? 0 : 1;
}
