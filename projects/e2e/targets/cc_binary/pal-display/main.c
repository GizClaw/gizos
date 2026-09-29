#include "h2_desktop_platform.h"
#include "h2_pal_display_e2e.h"
#include "h2_sdl3.h"
#include "h2_smoke_host_runtime.h"
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
  h2_sdl3_t *provider;
  const uint16_t *expected;
  int width, height, seen, rc;
  uint32_t brightness;
} observer_t;
static void captured(void *user, const h2_sdl3_render_frame_t *frame) {
  observer_t *o = user;
  if (!o->expected)
    return;
  o->seen = 1;
  o->rc =
      frame->width != (uint32_t)o->width || frame->height != (uint32_t)o->height
          ? H2_DISPLAY_ERR_IO
          : h2_pal_display_e2e_compare(frame->rgba, frame->stride_bytes, 0,
                                       o->expected, o->width, o->height,
                                       o->brightness, 3);
}
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  observer_t *o = user;
  o->expected = pixels;
  o->width = width;
  o->height = height;
  o->brightness = brightness;
  o->seen = 0;
  h2_sdl3_event_t event;
  for (unsigned i = 0; i < 100 && !o->seen; ++i) {
    (void)h2_sdl3_poll_event(o->provider, &event);
    if (!o->seen)
      h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 10);
  }
  o->expected = NULL;
  printf("H2_DISPLAY_OBSERVATION case=%s source=SDL_RenderReadPixels seen=%d "
         "rc=%d brightness=%u\n",
         id, o->seen, o->rc, brightness);
  return o->seen ? o->rc : H2_PAL_ERR_TIMEOUT;
}
int main(void) {
  const h2_sdl3_config_t surface = {
      .title = "PAL Display E2E", .width = 96, .height = 80};
  h2_sdl3_t *provider = NULL;
  int rc = h2_sdl3_create(&surface, &provider);
  if (rc)
    return 2;
  observer_t observer = {.provider = provider};
  rc = h2_sdl3_set_render_capture(provider, captured, &observer);
  const h2_pal_display_api_t *display = h2_sdl3_display(provider);
#ifdef H2_DISPLAY_EXPECT_NOOP_REJECTION
  /* Focused negative test: keep the real SDL renderer, but make one supported
   * format return success without drawing. This must fail qualification. */
  h2_pal_display_vtable_t altered_vtable = *display->vtable;
  real_draw = altered_vtable.draw_bitmap;
  altered_vtable.draw_bitmap = noop_rgb444;
  h2_pal_display_api_t altered_display = *display;
  altered_display.vtable = &altered_vtable;
  display = &altered_display;
#endif
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "pal-display", "desktop", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      display);
  h2_runtime_t *runtime = NULL;
  if (!rc)
    rc = h2_runtime_init(&config, &runtime);
  h2_pal_display_e2e_result_t result = {0};
  const h2_pal_display_e2e_config_t test = {.supported_formats = 7,
                                            .clips_rectangles = 1,
                                            .observe = observe,
                                            .user = &observer};
  if (!rc)
    rc = h2_pal_display_e2e_run(runtime, &test, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = h2_sdl3_set_render_capture(provider, NULL, NULL);
  h2_sdl3_destroy(provider);
  h2_pal_display_e2e_print(&result, "macos", rc, teardown);
#ifdef H2_DISPLAY_EXPECT_NOOP_REJECTION
  return rc == H2_DISPLAY_ERR_IO && result.failed == 1 &&
                 !result.qualified && !result.cleanup && !teardown &&
                 result.cases[9].ran &&
                 result.cases[9].result == H2_DISPLAY_ERR_IO
             ? 0
             : 1;
#else
  return rc || teardown || !result.qualified ? 1 : 0;
#endif
}
