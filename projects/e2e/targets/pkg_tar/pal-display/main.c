#include "h2_pal_display_e2e.h"
#include "h2_web_main_thread.h"
#include "h2_web_application_host.h"
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
static h2_pal_display_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  const h2_pal_display_e2e_config_t test = {
      .supported_formats = 1, .clips_rectangles = 0, .observe = observe};
  return h2_pal_display_e2e_run(runtime, &test, &result);
}
int main(void) {
  const h2_web_application_host_config_t config = {
      .name = "pal-display", .display_width = 96, .display_height = 80};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
  h2_pal_display_e2e_print(&result, "wasm-chromium", rc, teardown);
  return rc || teardown || lifecycle.retained || !result.qualified ? 1 : 0;
}
