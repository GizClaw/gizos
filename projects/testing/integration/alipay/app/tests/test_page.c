#include "h2_alipay_e2e.h"
#include "h2_desktop_platform.h"
#include "h2_qrcode.h"
#include "h2_smoke_host_runtime.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct page_fixture {
  int width, height;
  pthread_t ui_thread;
  uint16_t* frame;
  char code[H2_ALIPAY_BINDING_CODE_CAPACITY];
  int prepare_rc, ready_rc, draw_rc, observe_rc, start_rc, code_rc;
  unsigned opens, closes, draws, presents, init_calls, code_calls, reports;
  unsigned joins;
  bool retry_join, joined;
  h2_pal_task_t* worker;
  h2_alipay_e2e_result_t result;
} page_fixture_t;

static void ui_thread(page_fixture_t* fixture) {
  assert(pthread_equal(pthread_self(), fixture->ui_thread));
}

static int display_open(void* user) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  ++fixture->opens;
  return H2_PAL_OK;
}

static int display_info(void* user, h2_display_info_t* info) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  *info = (h2_display_info_t){fixture->width, fixture->height,
                              H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}

static int draw(void* user, const h2_display_rect_t* rect, const void* pixels,
                size_t stride, h2_display_pixel_format_t format) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  ++fixture->draws;
  assert(format == H2_DISPLAY_PIXEL_RGB565 && rect->x >= 0 && rect->y >= 0);
  assert(rect->x + rect->width <= fixture->width);
  assert(rect->y + rect->height <= fixture->height);
  if (fixture->draw_rc != H2_PAL_OK) return fixture->draw_rc;
  for (int row = 0; row < rect->height; ++row)
    memcpy(fixture->frame + (size_t)(row + rect->y) * fixture->width + rect->x,
           (const uint8_t*)pixels + (size_t)row * stride,
           (size_t)rect->width * 2u);
  return H2_PAL_OK;
}

static int present(void* user) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  ++fixture->presents;
  return H2_PAL_OK;
}

static int brightness(void* user, uint32_t percent) {
  ui_thread(user);
  assert(percent <= 100u);
  return H2_PAL_OK;
}

static int display_close(void* user) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  ++fixture->closes;
  return H2_PAL_OK;
}

static int task_start(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out_task) {
  page_fixture_t* fixture = user;
  bool sdk = strcmp(options->name, "alipay/e2e/sdk") == 0;
  if (sdk && fixture->start_rc != H2_PAL_OK) return fixture->start_rc;
  int result = h2_pal_task_start(h2_desktop_platform_task_api(), options, entry,
                                 context, out_task);
  if (result == H2_PAL_OK && sdk) fixture->worker = *out_task;
  return result;
}

static int task_join(void* user, h2_pal_task_t* task) {
  page_fixture_t* fixture = user;
  bool sdk = task == fixture->worker;
  if (sdk) {
    ++fixture->joins;
    if (fixture->retry_join && fixture->joins == 1u) return H2_PAL_ERR_BUSY;
  }
  int result = h2_pal_task_join(h2_desktop_platform_task_api(), task);
  if (sdk && result == H2_PAL_OK) fixture->joined = true;
  return result;
}

static h2_pal_result_t prepare(h2_runtime_t* runtime, void* user) {
  (void)runtime;
  page_fixture_t* fixture = user;
  assert(!pthread_equal(pthread_self(), fixture->ui_thread));
  return fixture->prepare_rc;
}

static h2_pal_result_t pre_init(void* user, int* sdk_rc, bool* first) {
  page_fixture_t* fixture = user;
  assert(!pthread_equal(pthread_self(), fixture->ui_thread));
  *first = fixture->init_calls++ == 0u;
  *sdk_rc = 0;
  return H2_PAL_OK;
}

static h2_pal_result_t ready(void* user, uint32_t feature,
                             h2_alipay_blocker_t* blocker) {
  (void)feature;
  page_fixture_t* fixture = user;
  *blocker = fixture->ready_rc == H2_PAL_OK ? H2_ALIPAY_BLOCKER_NONE
                                            : H2_ALIPAY_BLOCKER_SECURITY_SCHEME;
  return fixture->ready_rc;
}

static h2_pal_result_t binding_status(void* user, bool* bound) {
  (void)user;
  *bound = false;
  return H2_PAL_OK;
}

static h2_pal_result_t binding_code(void* user, uint32_t feature, char* code,
                                    size_t capacity, size_t* length,
                                    int* sdk_rc) {
  (void)feature;
  page_fixture_t* fixture = user;
  ++fixture->code_calls;
  *length = strlen(fixture->code);
  assert(*length < capacity);
  memcpy(code, fixture->code, *length + 1u);
  *sdk_rc = fixture->code_rc;
  return fixture->code_rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t observe(void* user, const uint8_t* pixels, size_t stride,
                               int width, int height) {
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  assert(fixture->joined && width == fixture->width &&
         height == fixture->height);
  for (int row = 0; row < height; ++row)
    assert(memcmp(pixels + (size_t)row * stride,
                  fixture->frame + (size_t)row * width,
                  (size_t)width * 2u) == 0);
  return fixture->observe_rc;
}

static void report(h2_runtime_t* runtime, void* user,
                   const h2_alipay_e2e_result_t* result, bool replay) {
  (void)runtime;
  page_fixture_t* fixture = user;
  ui_thread(fixture);
  assert(fixture->joined && !replay);
  fixture->result = *result;
  ++fixture->reports;
}

static bool should_exit(void* user) {
  return ((page_fixture_t*)user)->reports != 0u;
}

static int run(page_fixture_t* fixture) {
  fixture->ui_thread = pthread_self();
  const h2_pal_display_vtable_t display_vtable = {
      display_open, display_info, draw, present, brightness, display_close};
  const h2_pal_display_t display = {fixture, &display_vtable};
  const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
  const h2_pal_task_api_t task = {fixture, &task_vtable};
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "test", "host", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      &display);
  config.task = &task;
  config.sync = h2_desktop_platform_sync_api();
  h2_runtime_t* runtime = NULL;
  assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  const h2_alipay_vtable_t vtable = {pre_init, ready, binding_status,
                                     binding_code};
  const h2_alipay_api_t api = {.user = fixture,
                               .vtable = &vtable,
                               .source = H2_ALIPAY_SOURCE_CONTROLLED};
  const h2_alipay_e2e_config_t app = {.alipay = &api,
                                      .user = fixture,
                                      .prepare = prepare,
                                      .observe = observe,
                                      .report = report,
                                      .should_exit = should_exit};
  int result = h2_alipay_e2e_run(runtime, &app);
  assert(h2_runtime_destroy(&runtime) == H2_PAL_OK);
  assert(fixture->opens == 1u && fixture->closes == 1u);
  return result;
}

static void verify_qr(const page_fixture_t* fixture) {
  uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(12)],
      scratch[sizeof(modules)];
  h2_qrcode_t qr;
  assert(h2_qrcode_encode_text(fixture->code, H2_QRCODE_ECC_MEDIUM, 12, modules,
                               sizeof(modules), scratch, sizeof(scratch),
                               &qr) == H2_PAL_OK);
  int available_width = fixture->width - 16;
  int available_height = fixture->height - 52;
  int side = qr.size + 8;
  int scale = (available_width < available_height ? available_width
                                                  : available_height) /
              side;
  assert(scale >= 2);
  int left = (fixture->width - side * scale) / 2;
  int top = 28 + (available_height - side * scale) / 2;
  assert(top >= 28 && top + side * scale <= fixture->height - 24);
  for (int row = 0; row < side * scale; ++row)
    for (int column = 0; column < side * scale; ++column) {
      int module_x = column / scale - 4;
      int module_y = row / scale - 4;
      uint16_t expected =
          h2_qrcode_module_is_dark(&qr, module_x, module_y) ? 0u : UINT16_MAX;
      assert(fixture->frame[(size_t)(top + row) * fixture->width + left +
                            column] == expected);
    }
}

static void save_frame(const page_fixture_t* fixture, const char* name) {
  const char* directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory == NULL) return;
  char path[4096];
  assert(snprintf(path, sizeof(path), "%s/%s.ppm", directory, name) > 0);
  FILE* file = fopen(path, "wb");
  assert(file != NULL);
  fprintf(file, "P6\n%d %d\n255\n", fixture->width, fixture->height);
  for (int index = 0; index < fixture->width * fixture->height; ++index) {
    uint16_t pixel = fixture->frame[index];
    const uint8_t rgb[3] = {(uint8_t)(((pixel >> 11) & 31u) * 255u / 31u),
                            (uint8_t)(((pixel >> 5) & 63u) * 255u / 63u),
                            (uint8_t)((pixel & 31u) * 255u / 31u)};
    assert(fwrite(rgb, 1u, sizeof(rgb), file) == sizeof(rgb));
  }
  assert(fclose(file) == 0);
}

int main(void) {
  uint16_t frame[320 * 320];
  const int dimensions[][2] = {{240, 240}, {320, 240}, {240, 320}};
  for (size_t index = 0; index < 3u; ++index) {
    page_fixture_t fixture = {.width = dimensions[index][0],
                              .height = dimensions[index][1],
                              .frame = frame,
                              .retry_join = true};
    memset(fixture.code, 'x', sizeof(fixture.code) - 1u);
    assert(run(&fixture) == H2_PAL_OK);
    assert(fixture.init_calls == 2u && fixture.code_calls == 1u);
    assert(fixture.joins == 2u && fixture.result.qr_visible);
    verify_qr(&fixture);
    if (index == 0u) save_frame(&fixture, "tiga-qr-maximum-code");
  }
  page_fixture_t blocked = {.width = 240,
                            .height = 240,
                            .frame = frame,
                            .ready_rc = H2_PAL_ERR_UNAVAILABLE,
                            .code_rc = -91};
  assert(run(&blocked) == H2_PAL_OK);
  assert(blocked.result.blocker == H2_ALIPAY_BLOCKER_NONE);
  assert(blocked.result.code_sdk_rc == -91);
  assert(blocked.code_calls == 1u && !blocked.result.qr_visible);
  save_frame(&blocked, "tiga-sdk-error");
  page_fixture_t offline = {.width = 240,
                            .height = 240,
                            .frame = frame,
                            .prepare_rc = H2_PAL_ERR_TIMEOUT};
  assert(run(&offline) == H2_PAL_OK);
  assert(offline.init_calls == 0u &&
         offline.result.stage == H2_ALIPAY_E2E_NETWORK);
  page_fixture_t small = {.width = 240, .height = 239, .frame = frame};
  assert(run(&small) == H2_PAL_ERR_UNSUPPORTED);
  assert(small.init_calls == 0u && small.draws == 0u);
  page_fixture_t failed = {
      .width = 240, .height = 240, .frame = frame, .draw_rc = H2_PAL_ERR_IO};
  assert(run(&failed) == H2_PAL_ERR_IO);
  assert(failed.init_calls == 0u && failed.reports == 0u);
  page_fixture_t start_failed = {.width = 240,
                                 .height = 240,
                                 .frame = frame,
                                 .start_rc = H2_PAL_ERR_NO_MEMORY};
  assert(run(&start_failed) == H2_PAL_ERR_NO_MEMORY);
  assert(start_failed.init_calls == 0u && start_failed.reports == 0u);
  page_fixture_t transport_failed = {
      .width = 240, .height = 240, .frame = frame, .observe_rc = H2_PAL_ERR_IO};
  strcpy(transport_failed.code, "https://controlled.invalid/code");
  assert(run(&transport_failed) == H2_PAL_ERR_IO);
  assert(!transport_failed.result.qr_visible);
  return 0;
}
