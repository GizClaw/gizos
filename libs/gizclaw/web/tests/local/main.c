#include "h2_web_gizclaw.h"
#include "h2_web_platform.h"
#include "h2_web_main_thread.h"
#include "h2_app_test_fs.h"
#include "h2_lua_apps.h"
#include <emscripten.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_web_platform_t *platform;
static const h2_pal_task_api_t *base_task;
static h2_pal_task_t *bridge_task;
static bool fail_bridge_join;
static _Atomic int reads, writes, entered, gate, completions, delivery;
static uint32_t brightness;
static bool enabled;
static unsigned char *archive;
static size_t archive_size;
static int start(void *u, const h2_pal_task_options_t *o,
                 h2_pal_task_entry_t f, void *ctx, h2_pal_task_t **out) {
  (void)u;
  int rc = h2_pal_task_start(base_task, o, f, ctx, out);
  if (!rc && !strcmp(o->name, "$gizclaw/web/device")) bridge_task = *out;
  return rc;
}
static int join(void *u, h2_pal_task_t *task) {
  (void)u;
  if (task == bridge_task && fail_bridge_join) {
    fail_bridge_join = false;
    return H2_PAL_ERR_IO;
  }
  return h2_pal_task_join(base_task, task);
}
static const h2_pal_task_vtable_t tasks_vtable = {start, join};
static const h2_pal_task_api_t tasks = {.vtable = &tasks_vtable};
EMSCRIPTEN_KEEPALIVE int h2_local_fixture_state(int field) {
  switch (field) {
  case 1: return atomic_load(&reads);
  case 2: return atomic_load(&writes);
  case 3: return atomic_load(&entered);
  case 4: return atomic_load(&completions);
  case 5: return atomic_load(&delivery);
  case 6: return (int)brightness;
  case 7: return enabled;
  default: return -1;
  }
}
EMSCRIPTEN_KEEPALIVE void h2_local_fixture_gate(int value) {
  atomic_store(&gate, value);
}
static int read_led(void *u, h2_gizclaw_mhs_read_t *out) {
  (void)u;
  ++reads;
  out->led = (gizclaw_rpc_v1_LedHwdReadResponse){.has_brightness_percent = true,
      .brightness_percent = brightness, .has_enabled = true, .enabled = enabled};
  return H2_PAL_OK;
}
static int write_led(void *u, const h2_gizclaw_mhs_write_t *in,
                     h2_gizclaw_mhs_read_t *out) {
  ++writes;
  if (in->led.has_brightness_percent) brightness = in->led.brightness_percent;
  if (in->led.has_enabled) enabled = in->led.enabled;
  return read_led(u, out);
}
static void complete(void *u, int rc) {
  (void)u;
  atomic_store(&delivery, rc);
  ++completions;
}
static int find(void *u, h2_gizclaw_tool_t tool, h2_gizclaw_rpc_bytes_t payload,
                h2_gizclaw_rpc_provider_response_t *out) {
  (void)u; (void)tool; (void)payload;
  atomic_store(&entered, 1);
  while (atomic_load(&gate))
    assert(h2_pal_time_sleep_ms(h2_web_platform_time_api(platform), 1) == 0);
  *out = (h2_gizclaw_rpc_provider_response_t){.on_complete = complete};
  return H2_PAL_OK;
}
static int error(void *u, h2_gizclaw_tool_t tool, h2_gizclaw_rpc_bytes_t payload,
                 h2_gizclaw_rpc_provider_response_t *out) {
  (void)u; (void)tool; (void)payload; (void)out;
  return -1234;
}
/* A bounded fixture HTTP PAL supplies an actual lua_app archive. The existing
 * GizClaw tool and existing installer perform all validation and file writes. */
static int http(void *u, const h2_pal_http_request_t *request,
                h2_pal_http_response_t *out) {
  (void)u;
  static const char uri[] = "https://fixture.invalid/demo";
  assert(request->url.len == sizeof(uri) - 1u &&
         !memcmp(request->url.data, uri, sizeof(uri) - 1u));
  assert(request->read_cb && request->chunk_buf_cap);
  out->status_code = 200;
  out->content_length = (int64_t)archive_size;
  for (size_t at = 0; at < archive_size;) {
    if (h2_pal_http_request_is_canceled(request)) return H2_PAL_ERR_CLOSED;
    size_t count = archive_size - at;
    if (count > request->chunk_buf_cap) count = request->chunk_buf_cap;
    int rc = request->read_cb(request->user, request, archive + at, count,
                              at + count, archive_size - at - count);
    if (rc) return rc;
    at += count;
  }
  int rc = request->read_cb(request->user, request, NULL, 0, archive_size, 0);
  out->body_len = archive_size;
  return rc;
}
static const h2_pal_http_vtable_t http_vtable = {.request = http};
static const h2_pal_http_api_t http_api = {.vtable = &http_vtable};
void h2_local_fixture_assertions(void *, h2_web_main_result_t *, h2_web_main_completion_t *);
void h2_local_fixture_closed(void *, h2_web_main_result_t *, h2_web_main_completion_t *);
int main(void) {
  platform = h2_web_platform_create(&(h2_web_platform_config_t){240, 240});
  assert(platform);
  base_task = h2_web_platform_task_api(platform);
  const h2_pal_mem_api_t *mem = h2_web_platform_mem_api();
  FILE *file = fopen("/fixtures/demo.tar.zlib", "rb");
  assert(file && !fseek(file, 0, SEEK_END));
  long size = ftell(file);
  assert(size > 0 && size < 1048576 && !fseek(file, 0, SEEK_SET));
  archive_size = (size_t)size;
  archive = h2_pal_mem_alloc(mem, archive_size);
  assert(archive && fread(archive, 1, archive_size, file) == archive_size);
  assert(!fclose(file));
  h2_app_test_fs_t fs = {0};
  assert(h2_app_test_fs_init(&fs, mem) == H2_PAL_OK);
  h2_lua_apps_t *apps = NULL;
  const h2_lua_apps_config_t catalog = {.allocator = mem, .fs = &fs.api,
      .sync = h2_web_platform_sync_api(platform), .install_root = "/apps"};
  assert(h2_lua_apps_create(&catalog, &apps) == H2_PAL_OK);
  const h2_gizclaw_tool_handler_t tools[] = {
      {H2_GIZCLAW_TOOL_DEVICE_FIND, find, NULL},
      {H2_GIZCLAW_TOOL_SOCIAL_PING, error, NULL}};
  const h2_gizclaw_mhs_device_t led = {.id = "led.fixture",
      .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED,
      .read = read_led, .write = write_led};
  const h2_gizclaw_config_t product = {.allocator = mem,
      .time = h2_web_platform_time_api(platform), .model = "local-fixture",
      .http = &http_api, .lua_apps = apps, .tool_handlers = tools,
      .tool_handler_count = 2, .mhs_devices = &led, .mhs_device_count = 1};
  const h2_gizclaw_service_config_t config = {.client_config = &product,
      .task = &tasks, .sync = h2_web_platform_sync_api(platform),
      .queue = h2_web_platform_queue_api(platform), .operation_capacity = 4,
      .client_poll_timeout_ms = 1};
  h2_gizclaw_service_t *service = NULL;
  assert(h2_gizclaw_service_init(&config, &service) == H2_PAL_OK);
  assert(h2_gizclaw_local_start(service) == H2_PAL_OK);
  h2_web_gizclaw_t *bridge = NULL;
  const h2_web_gizclaw_config_t web = {.service = service, .allocator = mem,
      .task = &tasks, .queue = config.queue};
  assert(h2_web_gizclaw_open(&web, &bridge) == H2_PAL_OK);
  int rc = h2_web_main_call(h2_local_fixture_assertions, NULL).i32;
  atomic_store(&gate, 0);
  fail_bridge_join = true;
  assert(h2_web_gizclaw_close(&bridge) == H2_PAL_ERR_IO && bridge);
  if (h2_web_main_call(h2_local_fixture_closed, NULL).i32) rc = 1;
  assert(h2_gizclaw_service_deinit(service) != H2_PAL_OK);
  assert(h2_web_gizclaw_close(&bridge) == H2_PAL_OK && !bridge);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(h2_lua_apps_destroy(apps) == H2_PAL_OK);
  assert(h2_app_test_fs_deinit(&fs) == H2_PAL_OK);
  h2_pal_mem_free(mem, archive);
  puts("LOCAL close_retry=PASS");
  int destroy = h2_web_platform_destroy(platform);
  printf("H2_WEB_APP name=gizclaw-local result=%s rc=%d destroy=%d\n",
         !rc && !destroy ? "PASS" : "FAIL", rc, destroy);
  return rc || destroy;
}
