/* This entry owns its fixture providers and validates explicit acquisition,
 * Button routing and failure cleanup without a Host hook table. */
#include "h2_smoke_host_runtime.h"
#include "h2_web_fs.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <assert.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define WEB_HARDWARE_COMPONENT 42u
#define WEB_HARDWARE_PERIPH 501u

typedef struct web_hardware {
  int prepared;
  int configured;
  volatile int edges;
  volatile int accepted;
  int mode, releases, entry_done;
  const h2_pal_fs_api_t *fs;
  h2_pal_fs_file_t *file;
} web_hardware_t;

/* clang-format off */
EM_JS(void, fixture_mode,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () =>
      Number(new URLSearchParams(location.search).get('mode') || 0));
});
/* clang-format on */

static const h2_pal_periph_single_button_payload_t k_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static h2_pal_periph_info_t button_info(void) {
  h2_pal_periph_info_t info = {
      .id = WEB_HARDWARE_PERIPH,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .payload = &k_button_payload,
      .payload_size = sizeof(k_button_payload),
  };
  (void)snprintf(info.name, sizeof(info.name), "action");
  return info;
}

static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  const h2_pal_periph_info_t info = button_info();
  return callback(callback_user, &info);
}

static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id != WEB_HARDWARE_PERIPH)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = button_info();
  return H2_PAL_OK;
}

static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = WEB_HARDWARE_COMPONENT,
      .periph_id = WEB_HARDWARE_PERIPH,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(void *user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t *out_id) {
  (void)user;
  if (out_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id != WEB_HARDWARE_COMPONENT)
    return H2_PAL_ERR_NOT_FOUND;
  *out_id = WEB_HARDWARE_PERIPH;
  return H2_PAL_OK;
}

static const h2_pal_periph_vtable_t k_periph_vtable = {
    .list = periph_list,
    .get = periph_get,
};
static const h2_pal_periph_api_t k_periph = {.vtable = &k_periph_vtable};
static const h2_runtime_component_mapper_vtable_t k_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};
static const h2_runtime_component_mapper_t k_mapper = {
    .vtable = &k_mapper_vtable,
};

static h2_pal_result_t configure_runtime(void *user,
                                         h2_runtime_config_t *config) {
  web_hardware_t *hardware = user;
  config->board = "web-hardware";
  config->periph = &k_periph;
  config->component_mapper = &k_mapper;
  hardware->configured = 1;
  hardware->fs = config->fs;
  const h2_pal_result_t rc = (h2_pal_result_t)h2_pal_fs_open(
      config->fs, "/hardware/source", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
      &hardware->file);
  return rc != H2_PAL_OK ? rc : hardware->mode == 1 ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t release_runtime(void *user) {
  web_hardware_t *hardware = user;
  assert(++hardware->releases == 1);
  assert(hardware->entry_done == (hardware->mode != 1));
  if (hardware->mode == 2)
    return H2_PAL_ERR_IO;
  const h2_pal_result_t rc =
      (h2_pal_result_t)h2_pal_fs_close(hardware->fs, hardware->file);
  if (rc == H2_PAL_OK)
    hardware->file = NULL;
  printf("H2_WEB_HARDWARE release=%d rc=%d\n", hardware->releases, (int)rc);
  return rc;
}

static h2_pal_result_t button(void *user, h2_runtime_t *runtime,
                              h2_runtime_component_id_t component_id,
                              int pressed) {
  web_hardware_t *hardware = user;
  if (component_id != WEB_HARDWARE_COMPONENT)
    return H2_PAL_ERR_NOT_FOUND;
  const h2_pal_result_t result = h2_runtime_button_push_edge(
      runtime, WEB_HARDWARE_PERIPH,
      pressed ? H2_RUNTIME_BUTTON_EDGE_DOWN : H2_RUNTIME_BUTTON_EDGE_UP);
  hardware->accepted += result == H2_PAL_OK;
  ++hardware->edges;
  return result;
}

/* clang-format off */
EM_JS(void, page_input,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32"], "i32", action => {
    if (action === 1) { globalThis.h2WebBindButton?.(0, "Enter", "action"); return 0; }
    if (action === 2) { globalThis.h2WebUnbindButtons?.(); return 0; }
    if (action === 3) return Module.h2WebStopRequested ? 1 : 0;
    return globalThis.h2WebTakeButtonEdge?.() || 0;
  });
});
/* clang-format on */
static int page_action(int action) {
  return h2_web_main_call(page_input, (const void *[]){&action}).i32;
}
int main(void) {
  static web_hardware_t hardware;
  hardware.mode = h2_web_main_call(fixture_mode, NULL).i32;
  const h2_web_platform_config_t platform_config = {240, 240};
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  if (platform == NULL)
    return 1;
  hardware.prepared = 1;
  const h2_web_fs_config_t fs_config = {.persistent_root = "/hardware"};
  h2_web_fs_t *fs = NULL;
  int rc = h2_web_fs_open(platform, &fs_config, &fs);
  h2_runtime_t *runtime = NULL;
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "web-hardware", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  config.task = h2_web_platform_task_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.fs = h2_web_fs_api(fs);
  if (rc == H2_PAL_OK)
    rc = configure_runtime(&hardware, &config);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_input_start(runtime, NULL);
  if (rc == H2_PAL_OK) {
    printf("H2_WEB_HARDWARE prepared=%d configured=%d board=%s\n",
           hardware.prepared, hardware.configured, runtime->board);
    (void)page_action(1);
    puts("H2_WEB_APP name=web-hardware stage=ready");
    while (hardware.mode == 0 && rc == H2_PAL_OK && hardware.edges < 2 &&
           !page_action(3)) {
      int edge = page_action(0);
      if (edge < 0)
        rc = H2_PAL_ERR_NO_SPACE;
      else if (edge != 0)
        rc = button(&hardware, runtime, WEB_HARDWARE_COMPONENT, edge & 1);
      else
        rc = h2_pal_time_sleep_ms(runtime->time, 10u);
    }
    (void)page_action(2);
    if (hardware.mode == 0) {
      printf("H2_WEB_HARDWARE edges=%d accepted=%d\n", hardware.edges,
             hardware.accepted);
      if (rc == H2_PAL_OK && hardware.accepted != 2)
        rc = H2_PAL_ERR_UNAVAILABLE;
    }
    hardware.entry_done = 1;
  }
  int released = release_runtime(&hardware);
  assert(hardware.releases == 1);
  if (hardware.mode == 2) {
    assert(released == H2_PAL_ERR_IO && hardware.file != NULL);
    h2_pal_fs_stat_t info = {0};
    assert(h2_pal_fs_stat(hardware.fs, "/hardware/source", &info) == H2_PAL_OK);
    printf("H2_WEB_APP name=web-hardware result=FAIL rc=%d retained=1\n",
           released);
  } else {
    assert(released == H2_PAL_OK && hardware.file == NULL);
    int stopped = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
    assert(stopped == H2_PAL_OK);
    h2_runtime_deinit(runtime);
    int fs_result = h2_web_fs_close(fs);
    assert(fs_result == H2_PAL_OK);
    int destroyed = h2_web_platform_destroy(platform);
    assert(destroyed == H2_PAL_OK);
    assert(rc == (hardware.mode == 1 ? H2_PAL_ERR_IO : H2_PAL_OK));
    printf("H2_WEB_APP name=web-hardware result=%s rc=%d fs=%d destroy=%d\n",
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc, fs_result, destroyed);
  }
  puts("H2_WEB_HARDWARE checks=PASS");
  return 0;
}
