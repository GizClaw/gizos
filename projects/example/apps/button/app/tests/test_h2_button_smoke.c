#include "h2_button_smoke.h"
#include "h2_desktop_platform.h"
#include "h2_runtime_test.h"
#include "h2_smoke_host_runtime.h"

#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

typedef struct fixture {
  atomic_size_t allocations;
  unsigned opened, closed, drawn, presented, started, logs, polls;
  h2_pal_result_t started_result;
  int draw_result, close_result;
  int width, height;
} fixture_t;

static void *allocate(void *user, size_t size) {
  void *ptr = malloc(size);
  if (ptr != NULL) ++((fixture_t *)user)->allocations;
  return ptr;
}
static void release(void *user, void *ptr) {
  if (ptr != NULL) {
    assert(((fixture_t *)user)->allocations > 0u);
    --((fixture_t *)user)->allocations;
    free(ptr);
  }
}
static void *resize(void *user, void *ptr, size_t size) {
  if (ptr == NULL) return allocate(user, size);
  if (size == 0u) { release(user, ptr); return NULL; }
  return realloc(ptr, size);
}
static int display_open(void *user) {
  ++((fixture_t *)user)->opened;
  return H2_PAL_OK;
}
static int display_info(void *user, h2_display_info_t *info) {
  fixture_t *f = user;
  *info = (h2_display_info_t){f->width, f->height, H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}
static int display_draw(void *user, const h2_display_rect_t *rect,
                        const void *pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  fixture_t *f = user;
  assert(pixels != NULL && rect->width > 0 && rect->height > 0);
  assert(stride == (size_t)rect->width * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  ++f->drawn;
  return f->draw_result;
}
static int display_present(void *user) {
  ++((fixture_t *)user)->presented;
  return H2_PAL_OK;
}
static int display_close(void *user) {
  fixture_t *f = user;
  ++f->closed;
  return f->close_result;
}
static int log_write(void *user, h2_pal_log_level_t level,
                     const char *scope, const char *message) {
  fixture_t *f = user;
  (void)level;
  if (strcmp(scope, "button-smoke") != 0) return H2_PAL_OK;
  static const char *const expected[] = {
      "button=Action component=1 event=down down=1 up=0 action=0",
      "button=Action component=1 event=up down=1 up=1 action=0",
      "button=Action component=1 event=action down=1 up=1 action=1",
  };
  assert(f->logs < 3u);
  assert(strcmp(message, expected[f->logs]) == 0);
  ++f->logs;
  return H2_PAL_OK;
}
static int should_stop(void *user) {
  fixture_t *f = user;
  assert(++f->polls < 1000u);
  return f->logs == 3u && f->presented != 0u;
}
static void started(void *user, h2_pal_result_t result) {
  fixture_t *f = user;
  ++f->started;
  f->started_result = result;
}
static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                 h2_pal_periph_info_t *info) {
  (void)user;
  if (id != 10u) return H2_PAL_ERR_NOT_FOUND;
  *info = (h2_pal_periph_info_t){
      .id = 10u, .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON};
  return H2_PAL_OK;
}
static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                  h2_pal_periph_cb_t cb, void *cb_user) {
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON) return H2_PAL_OK;
  h2_pal_periph_info_t info;
  assert(periph_get(user, 10u, &info) == H2_PAL_OK);
  return cb(cb_user, &info);
}
static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t cb, void *cb_user) {
  (void)user;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON) return H2_PAL_OK;
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = 1u, .periph_id = 10u};
  return cb(cb_user, &entry);
}

int main(void) {
  fixture_t f = {.width = 640, .height = 480};
  const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = allocate, .realloc = resize, .free = release};
  const h2_pal_mem_api_t mem = {&f, &mem_vtable};
  const h2_pal_display_vtable_t display_vtable = {
      .open = display_open, .get_info = display_info,
      .draw_bitmap = display_draw, .present = display_present,
      .close = display_close};
  const h2_pal_display_api_t display = {&f, &display_vtable};
  const h2_pal_log_vtable_t log_vtable = {.write = log_write};
  const h2_pal_log_api_t log = {&f, &log_vtable};
  const h2_pal_periph_vtable_t periph_vtable = {
      .get = periph_get, .list = periph_list};
  const h2_pal_periph_api_t periph = {NULL, &periph_vtable};
  const h2_runtime_component_mapper_vtable_t mapper_vtable = {
      .list = mapper_list};
  const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};
  h2_runtime_config_t rc = h2_smoke_host_runtime_config(
      "test", "desktop", "host", &mem, h2_desktop_platform_time_api(),
      h2_desktop_platform_queue_api(), &display);
  rc.log = &log;
  rc.task = h2_desktop_platform_task_api();
  rc.sync = h2_desktop_platform_sync_api();
  rc.periph = &periph;
  rc.component_mapper = &mapper;
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&rc, &runtime) == H2_PAL_OK);
  h2_runtime_test_control_t *control = NULL;
  assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
  const size_t baseline_allocations = f.allocations;
  const h2_button_smoke_button_t button = {1u, "Action"};
  const h2_button_smoke_config_t config = {
      .width = 640u, .height = 480u, .buttons = &button, .button_count = 1u,
      .should_stop = should_stop, .stop_user = &f,
      .on_started = started, .started_user = &f};
  assert(h2_button_smoke_run(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_button_smoke_run(runtime, NULL) == H2_PAL_ERR_INVALID_ARG);
  h2_button_smoke_config_t invalid = config;
  invalid.buttons = NULL;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid = config; invalid.should_stop = NULL;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  const h2_button_smoke_button_t unnamed[] = {{1u, "Action"}, {2u, NULL}};
  invalid = config; invalid.buttons = unnamed; invalid.button_count = 2u;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid = config; invalid.button_count = 0u;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid.button_count = 17u;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid = config; invalid.width = 65536u; invalid.height = 65536u;
  assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
  assert(f.opened == 0u && f.started == 0u);
  for (unsigned axis = 0u; axis < 2u; ++axis) {
    invalid = config;
    if (axis == 0u) ++invalid.width; else ++invalid.height;
    assert(h2_button_smoke_run(runtime, &invalid) == H2_PAL_ERR_INVALID_ARG);
    assert(f.opened == axis + 1u && f.closed == f.opened);
    assert(f.started == axis + 1u && f.started_result == H2_PAL_ERR_INVALID_ARG);
    assert(f.allocations == baseline_allocations);
  }
  f.opened = f.closed = f.started = 0u;
  h2_runtime_component_info_t info;
  assert(h2_runtime_component_get(runtime, 1u, &info) == H2_PAL_OK);
  assert(info.kind == H2_RUNTIME_COMPONENT_BUTTON);
  assert(h2_runtime_test_button_down(control, 1u, 100u) == H2_PAL_OK);
  assert(h2_runtime_test_button_up(control, 1u, 100u, 200u) == H2_PAL_OK);
  assert(h2_runtime_test_button_action(control, 1u, 100u, 200u, 200u) == H2_PAL_OK);
  assert(h2_button_smoke_run(runtime, &config) == H2_PAL_OK);
  assert(f.logs == 3u && f.drawn > 0u && f.presented > 0u);
  assert(f.started == 1u && f.started_result == H2_PAL_OK);
  assert(f.opened == 1u && f.closed == 1u);
  /* libs/lvgl's lv_deinit keeps its global OS mutex (2 allocations per init). */
  const size_t retained_per_init = 2u;
  assert(f.allocations <= baseline_allocations + retained_per_init);
  /* A flush error wins over a later close error. */
  f.logs = f.polls = f.presented = 0u;
  f.draw_result = H2_PAL_ERR_IO;
  f.close_result = H2_PAL_ERR_UNAVAILABLE;
  assert(h2_button_smoke_run(runtime, &config) == H2_PAL_ERR_IO);
  assert(f.closed == 2u);
  assert(f.allocations <= baseline_allocations + 2u * retained_per_init);
  h2_runtime_test_control_close(control);
  h2_runtime_deinit(runtime);
  assert(f.allocations <= 2u * retained_per_init);
  return 0;
}
