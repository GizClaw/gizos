#include "h2_desktop_platform.h"
#include "h2_pal_display_e2e.h"
#include "h2_sdl3.h"
#include "h2_desktop_application_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef H2_DISPLAY_EXPECT_NOOP_REJECTION
static int (*real_draw)(void *, const h2_display_rect_t *, const void *, size_t,
                        h2_display_pixel_format_t);
static int noop_rgb444(void *user, const h2_display_rect_t *rect,
                       const void *pixels, size_t stride,
                       h2_display_pixel_format_t format) {
  return format == H2_DISPLAY_PIXEL_RGB444
             ? 0
             : real_draw(user, rect, pixels, stride, format);
}
#endif
typedef struct observer {
  pthread_mutex_t mutex;
  const uint16_t *expected;
  int width, height, seen, rc;
  uint32_t brightness;
} observer_t;
static void captured(void *user, const h2_sdl3_render_frame_t *frame) {
  observer_t *o = user;
  pthread_mutex_lock(&o->mutex);
  if (!o->expected) { pthread_mutex_unlock(&o->mutex); return; }
  o->seen = 1;
  o->rc =
      frame->width != (uint32_t)o->width || frame->height != (uint32_t)o->height
          ? H2_DISPLAY_ERR_IO
          : h2_pal_display_e2e_compare(frame->rgba, frame->stride_bytes, 0,
                                       o->expected, o->width, o->height,
                                       o->brightness, 3);
  pthread_mutex_unlock(&o->mutex);
}
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  observer_t *o = user;
  pthread_mutex_lock(&o->mutex);
  o->expected = pixels;
  o->width = width;
  o->height = height;
  o->brightness = brightness;
  o->seen = 0;
  pthread_mutex_unlock(&o->mutex);
  for (unsigned i = 0; i < 100; ++i) {
    pthread_mutex_lock(&o->mutex);
    int seen = o->seen;
    pthread_mutex_unlock(&o->mutex);
    if (seen) break;
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 10);
  }
  pthread_mutex_lock(&o->mutex);
  o->expected = NULL;
  pthread_mutex_unlock(&o->mutex);
  printf("H2_DISPLAY_OBSERVATION case=%s source=SDL_RenderReadPixels seen=%d "
         "rc=%d brightness=%u\n",
         id, o->seen, o->rc, brightness);
  return o->seen ? o->rc : H2_PAL_ERR_TIMEOUT;
}
static observer_t observer = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static h2_pal_display_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  const h2_pal_display_api_t *display = runtime->display;
#ifdef H2_DISPLAY_EXPECT_NOOP_REJECTION
  /* Focused negative test: keep the real SDL renderer, but make one supported
   * format return success without drawing. This must fail qualification. */
  static h2_pal_display_vtable_t altered_vtable;
  altered_vtable = *display->vtable;
  real_draw = altered_vtable.draw_bitmap;
  altered_vtable.draw_bitmap = noop_rgb444;
  static h2_pal_display_api_t altered_display;
  altered_display = *display;
  altered_display.vtable = &altered_vtable;
  display = &altered_display;
#endif
  const h2_pal_display_api_t *original = runtime->display;
  runtime->display = display;
  const h2_pal_display_e2e_config_t test = {.supported_formats = 7,
                                            .clips_rectangles = 1,
                                            .observe = observe,
                                            .user = &observer};
  int rc = h2_pal_display_e2e_run(runtime, &test, &result);
  runtime->display = original;
  return rc;
}
int main(void) {
  const h2_desktop_application_host_config_t platform = {
      .window = {.title = "PAL Display E2E", .width = 96, .height = 80},
      .user = &observer, .render_capture = captured};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_desktop_application_host_run(&platform, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
  h2_pal_display_e2e_print(&result, "macos", rc, teardown);
#ifdef H2_DISPLAY_EXPECT_NOOP_REJECTION
  return rc == H2_DISPLAY_ERR_IO && result.failed == 1 &&
                 !result.qualified && !result.cleanup && !teardown &&
                 result.cases[9].ran &&
                 result.cases[9].result == H2_DISPLAY_ERR_IO
             ? 0
             : 1;
#else
  return rc || teardown || lifecycle.retained || !result.qualified ? 1 : 0;
#endif
}
