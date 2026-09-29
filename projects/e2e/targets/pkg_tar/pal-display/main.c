#include "h2_pal_display_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten/threading.h>
#include <stdio.h>
/* Browser verifier captures the composited page, including the CSS filter.
 * It never treats getImageData or an input-buffer hash as brightness proof. */
/* clang-format off */
EM_JS(void, capture_request,(void *context,h2_web_main_result_t *result,h2_web_main_completion_t *completion),{
 h2WebMain(context,result,completion,["pointer","i32","i32","u32","pointer"],"i32",
  async (pixels,width,height,brightness,id) => {
   if (window.h2DisplayPending) return -9;
   const expected = Array.from(HEAPU16.subarray(pixels/2,pixels/2+width*height));
   return await new Promise(resolve => {
    const timer = setTimeout(() => { window.h2DisplayPending=null; resolve(-6); },30000);
    window.h2DisplayPending = {expected,width,height,brightness,id:UTF8ToString(id),
      complete:rc => { clearTimeout(timer);window.h2DisplayPending=null;resolve(rc); }};
    console.log('H2_DISPLAY_OBSERVE '+UTF8ToString(id));
   });
  });
});
/* clang-format on */
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  (void)user;
  return h2_web_main_call(
             capture_request,
             (const void *[]){&pixels, &width, &height, &brightness, &id})
      .i32;
}
int main(void) {
  if (emscripten_is_main_runtime_thread())
    return 2;
  const h2_web_platform_config_t config = {.display_width = 96,
                                           .display_height = 80};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (!platform)
    return 3;
  h2_runtime_config_t cfg = h2_smoke_host_runtime_config(
      "pal-display", "web", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&cfg, &runtime);
  h2_pal_display_e2e_result_t result = {0};
  const h2_pal_display_e2e_config_t test = {
      .supported_formats = 1, .clips_rectangles = 0, .observe = observe};
  if (!rc)
    rc = h2_pal_display_e2e_run(runtime, &test, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = h2_web_platform_destroy(platform);
  h2_pal_display_e2e_print(&result, "wasm-chromium", rc, teardown);
  return rc || teardown || !result.qualified ? 1 : 0;
}
