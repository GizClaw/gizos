#include "h2_bloomspeaker.h"

#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"

#include <assert.h>

typedef struct fixture {
  size_t display_open;
  size_t draw;
  size_t present;
  size_t display_close;
  size_t ready;
} fixture_t;

static int display_open(void *user) {
  ((fixture_t *)user)->display_open++;
  return H2_PAL_OK;
}

static int display_info(void *user, h2_display_info_t *out_info) {
  (void)user;
  *out_info = (h2_display_info_t){368, 448, H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}

static int draw_bitmap(void *user, const h2_display_rect_t *rect,
                       const void *pixels, size_t stride,
                       h2_display_pixel_format_t format) {
  fixture_t *fixture = user;
  assert(rect->x >= 0 && rect->y >= 0 && rect->width > 0 && rect->height > 0);
  assert(rect->x + rect->width <= 368 && rect->y + rect->height <= 448);
  assert(pixels != NULL && stride == 368u * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  fixture->draw++;
  return H2_PAL_OK;
}

static int display_present(void *user) {
  ((fixture_t *)user)->present++;
  return H2_PAL_OK;
}

static int display_close(void *user) {
  ((fixture_t *)user)->display_close++;
  return H2_PAL_OK;
}

static int should_stop(void *user) { return ((fixture_t *)user)->ready != 0u; }

static h2_pal_result_t ready(void *user) {
  ((fixture_t *)user)->ready++;
  return H2_PAL_OK;
}

int main(void) {
  static const h2_pal_display_vtable_t display_vtable = {
      .open = display_open,
      .get_info = display_info,
      .draw_bitmap = draw_bitmap,
      .present = display_present,
      .close = display_close,
  };
  fixture_t fixture = {0};
  const h2_pal_display_api_t display = {&fixture, &display_vtable};
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "test", "desktop", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      &display);
  runtime_config.log = h2_desktop_platform_log_api();
  runtime_config.task = h2_desktop_platform_task_api();
  runtime_config.sync = h2_desktop_platform_sync_api();
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&runtime_config, &runtime) == H2_PAL_OK);
  assert(h2_bloomspeaker_run(
             runtime, &(h2_bloomspeaker_config_t){
                          .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                          .power_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                          .pairing_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                          .should_stop = should_stop,
                          .should_stop_user = &fixture,
                          .on_ready = ready,
                          .on_ready_user = &fixture,
                      }) == H2_PAL_OK);
  assert(fixture.ready == 1u);
  assert(fixture.display_open == 1u && fixture.display_close == 1u);
  assert(fixture.draw >= 1u && fixture.present >= 1u);
  h2_runtime_deinit(runtime);
  return 0;
}
