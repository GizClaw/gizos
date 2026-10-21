/* Compile and call the production Lua entry. Wrappers inject faults through
 * real provider APIs; the fixture owns an extra FS borrowed by the Runtime. */
#include "h2_lua.h"
#include "h2_lua_job.h"
#include "h2_lua_task_names.h"
#include "h2_web_fs.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <assert.h>
#include <emscripten.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const uint8_t* test_source;
size_t test_source_size;
static int mode;
static h2_web_platform_t* platform;
static h2_runtime_t* runtime;
static h2_web_fs_t* fixture_fs;
static const h2_pal_fs_api_t* retained_fs;
static h2_pal_fs_file_t* retained_file;
static h2_lua_host_t* lua_host;
static const h2_pal_display_api_t* base_display;
static const h2_pal_task_api_t* base_tasks;
static h2_pal_display_api_t display;
static h2_pal_task_api_t tasks;
static h2_pal_task_t *display_task, *input_task;
static _Atomic unsigned closes, presents, failed_joins;
static int display_open(void* user) {
  assert(user == &display);
  return h2_pal_display_open(base_display);
}
static int display_info(void* user, h2_display_info_t* info) {
  assert(user == &display);
  return h2_pal_display_get_info(base_display, info);
}
static int display_bitmap(void* user, const h2_display_rect_t* rect,
                          const void* pixels, size_t stride,
                          h2_display_pixel_format_t format) {
  assert(user == &display);
  return h2_pal_display_draw_bitmap(base_display, rect, pixels, stride, format);
}
static int display_present(void* user) {
  assert(user == &display);
  atomic_fetch_add(&presents, 1u);
  if (mode == 4) {
    /* Exceed the old 2-second forced cancellation grace period. */
    assert(h2_pal_time_sleep_ms(runtime->time, 2400u) == H2_PAL_OK);
  }
  int rc = h2_pal_display_present(base_display);
  return mode == 2 ? H2_PAL_ERR_IO : rc;
}
static int display_close(void* user) {
  assert(user == &display);
  atomic_fetch_add(&closes, 1u);
  return h2_pal_display_close(base_display);
}
static const h2_pal_display_vtable_t display_vtable = {
    .open = display_open,
    .get_info = display_info,
    .draw_bitmap = display_bitmap,
    .present = display_present,
    .close = display_close,
};
static int task_start(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out) {
  assert(user == &tasks);
  int rc = h2_pal_task_start(base_tasks, options, entry, context, out);
  if (rc == H2_PAL_OK && strcmp(options->name, "$lua/display") == 0)
    display_task = *out;
  if (rc == H2_PAL_OK && strcmp(options->name, "$runtime/input") == 0)
    input_task = *out;
  return rc;
}
static int task_join(void* user, h2_pal_task_t* task) {
  assert(user == &tasks);
  if ((mode == 3 && task == display_task) ||
      ((mode == 5 || mode == 6) && task == input_task)) {
    atomic_fetch_add(&failed_joins, 1u);
    return mode == 6 ? H2_PAL_EXIT : H2_PAL_ERR_TASK;
  }
  return h2_pal_task_join(base_tasks, task);
}
static const h2_pal_task_vtable_t task_vtable = {
    .start = task_start,
    .join = task_join,
};

static h2_web_platform_t* create_platform(
    const h2_web_platform_config_t* config) {
  platform = h2_web_platform_create(config);
  assert(platform != NULL);
  const h2_web_fs_config_t fs_config = {.persistent_root = "/lifecycle"};
  assert(h2_web_fs_open(platform, &fs_config, &fixture_fs) == H2_PAL_OK);
  retained_fs = h2_web_fs_api(fixture_fs);
  return platform;
}
static h2_pal_result_t create_runtime(const h2_runtime_config_t* config,
                                      h2_runtime_t** out) {
  h2_runtime_config_t copy = *config;
  base_display = copy.display;
  base_tasks = copy.task;
  display = (h2_pal_display_api_t){.user = &display, .vtable = &display_vtable};
  tasks = (h2_pal_task_api_t){.user = &tasks, .vtable = &task_vtable};
  copy.display = &display;
  copy.task = &tasks;
  copy.fs = retained_fs;
  h2_pal_result_t rc = h2_runtime_init(&copy, out);
  runtime = *out;
  return rc;
}
static h2_pal_result_t create_lua(const h2_lua_host_config_t* config,
                                  h2_lua_host_t** out) {
  h2_pal_result_t rc = h2_lua_host_create(config, out);
  lua_host = *out;
  return rc;
}
static h2_pal_result_t destroy_platform(h2_web_platform_t* value) {
  assert(value == platform);
  if (mode == 7) {
    assert(h2_pal_fs_open(retained_fs, "/lifecycle/held",
                          H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &retained_file) == H2_PAL_OK);
  }
  h2_pal_result_t rc = h2_web_fs_close(fixture_fs);
  if (rc == H2_PAL_ERR_BUSY || rc == H2_PAL_ERR_TIMEOUT) return rc;
  assert(rc == H2_PAL_OK);
  fixture_fs = NULL;
  return h2_web_platform_destroy(value);
}
#define h2_web_platform_create create_platform
#define h2_runtime_init create_runtime
#define h2_lua_host_create create_lua
#define h2_web_platform_destroy destroy_platform
#define main lua_app_main
#include "libs/lua/web/src/h2_web_lua_app.c"
#undef main
#undef h2_web_platform_create
#undef h2_runtime_init
#undef h2_lua_host_create
#undef h2_web_platform_destroy
static const char normal_script[] =
    "local d,r=require('display'),require('runtime');d.clear('red');"
    "assert(d.submit());local s,e=d.flush();while not s do "
    "if e~=d.BUSY then local n,c=d.submit();assert(n==nil and c==e);return end;"
    "r.sleep(1);s,e=d.flush() end;assert(s.error==0)";
static const char stop_script[] =
    "local d,r=require('display'),require('runtime');d.clear('blue');"
    "assert(d.submit());while true do r.sleep(1) end";

/* clang-format off */
EM_JS(void, read_mode,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () =>
      Number(new URLSearchParams(location.search).get('mode') || 0));
});
/* clang-format on */

int main(void) {
  mode = h2_web_main_call(read_mode, NULL).i32;
  test_source = (const uint8_t*)(mode == 1   ? "error('ordinary script error')"
                                 : mode == 4 ? stop_script
                                             : normal_script);
  test_source_size = strlen((const char*)test_source);
  double began = emscripten_get_now();
  int rc = lua_app_main();
  if (mode == 7) {
    assert(rc == 2 && owned_platform == platform && owned_runtime == NULL);
    h2_pal_fs_stat_t info;
    assert(h2_pal_fs_stat(retained_fs, "/lifecycle/held", &info) == H2_PAL_OK);
    assert(retained_file != NULL);
    assert(lua_app_main() == 2);
  } else if (mode == 2 || mode == 3 || mode == 5 || mode == 6) {
    assert(rc == 2 && owned_platform == platform && owned_runtime == runtime);
    assert(lua_app_main() == 2);
    h2_web_platform_resource_stats_t stats;
    assert(h2_web_platform_get_resource_stats(platform, &stats) == H2_PAL_OK);
    assert(stats.live_mutexes > 0 && stats.allocations > 0);
    h2_pal_periph_info_t peripheral;
    assert(h2_pal_periph_get(runtime->periph, 1, &peripheral) == H2_PAL_OK);
    assert(strcmp(peripheral.name, "lifecycle") == 0);
    h2_pal_fs_stat_t info;
    assert(h2_pal_fs_stat(runtime->fs, "/lifecycle", &info) == H2_PAL_OK);
    if (mode == 2 || mode == 3) {
      assert(owned_lua == lua_host);
      h2_lua_job_id_t id;
      assert(h2_lua_job_submit_resource(lua_host, NULL, "app.lua", NULL, 0,
                                        &id) != H2_PAL_OK);
      assert(h2_lua_host_destroy_checked(lua_host) != H2_PAL_OK);
      h2_lua_job_status_t status;
      assert(h2_lua_job_get_status(lua_host, 1, &status) == H2_PAL_OK);
      printf("LIFECYCLE retained_vm=%zu ", status.memory_used);
    } else {
      assert(owned_lua == NULL && atomic_load(&failed_joins) > 0);
    }
    assert(mode != 2 || atomic_load(&closes) == 0);
    assert(mode != 3 || atomic_load(&closes) == 1);
    printf(
        "LIFECYCLE retained_tasks=%zu stacks=%zu allocations=%zu bytes=%zu "
        "joins_failed=%u\n",
        stats.live_tasks, stats.task_stack_bytes, stats.allocations,
        stats.allocation_bytes, atomic_load(&failed_joins));
  } else {
    assert(rc == (mode == 1 ? 1 : 0));
    assert(owned_platform == NULL && owned_runtime == NULL &&
           owned_lua == NULL);
    assert(mode != 4 || (atomic_load(&presents) == 1 &&
                         emscripten_get_now() - began >= 2400.0));
    mode = 0;
    test_source = (const uint8_t*)normal_script;
    test_source_size = strlen(normal_script);
    assert(lua_app_main() == 0);
  }
  puts("LIFECYCLE checks=PASS");
  return 0;
}
