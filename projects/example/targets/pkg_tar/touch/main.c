#include "h2_smoke_host_runtime.h"
#include "h2_touch_smoke.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <stdio.h>

static double stop_at;
/* clang-format off */
EM_JS(void, page_stop,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () => Module.h2WebStopRequested ? 1 : 0);
});
/* clang-format on */

static h2_runtime_t* input_runtime;
static int input_error;
static const h2_pal_periph_single_button_payload_t button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE};
static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id != 1u) return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_periph_info_t){.id = 1u,
                                .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
                                .name = "action",
                                .payload = &button_payload,
                                .payload_size = sizeof(button_payload)};
  return H2_PAL_OK;
}
static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t cb, void* cb_user) {
  if (cb == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  h2_pal_periph_info_t info;
  (void)periph_get(user, 1u, &info);
  return cb(cb_user, &info);
}
static h2_pal_result_t mapper_get(void* user, h2_runtime_component_id_t id,
                                  h2_pal_periph_id_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id != 1u) return H2_PAL_ERR_NOT_FOUND;
  *out = 1u;
  return H2_PAL_OK;
}
static h2_pal_result_t mapper_list(void* user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t cb,
                                   void* cb_user) {
  (void)user;
  if (cb == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  const h2_runtime_component_mapping_entry_t entry = {1u, 1u};
  return cb(cb_user, &entry);
}
static const h2_pal_periph_vtable_t periph_vtable = {periph_list, periph_get};
static const h2_pal_periph_api_t periph = {.vtable = &periph_vtable};
static const h2_runtime_component_mapper_vtable_t mapper_vtable = {mapper_list,
                                                                   mapper_get};
static const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};
/* The page queues edges; only this Worker calls the Runtime. */
/* clang-format off */
EM_JS(void, page_input,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32"], "i32", action => {
    if (action === 1) { globalThis.h2WebBindButton?.(0, "Enter", "action"); return 0; }
    if (action === 2) { globalThis.h2WebUnbindButtons?.(); return 0; }
    return globalThis.h2WebTakeButtonEdge?.() || 0;
  });
});
/* clang-format on */
static int should_stop(void* user) {
  (void)user;
  int edge;
  while (
      (edge = h2_web_main_call(page_input, (const void*[]){&(int){0}}).i32) !=
      0) {
    input_error = edge < 0 ? H2_PAL_ERR_NO_SPACE
                           : h2_runtime_button_push_edge(
                                 input_runtime, 1u,
                                 (edge & 1) ? H2_RUNTIME_BUTTON_EDGE_DOWN
                                            : H2_RUNTIME_BUTTON_EDGE_UP);
    if (input_error != H2_PAL_OK) return 1;
  }
  return h2_web_main_call(page_stop, NULL).i32 ||
         (stop_at > 0.0 && emscripten_get_now() >= stop_at);
}

static void touch_ready(void* user, h2_pal_result_t result) {
  (void)user;
  printf("H2_WEB_TOUCH_READY rc=%d\n", result);
}
int main(void) {
  const h2_web_platform_config_t config = {800, 480};
  h2_web_platform_t* platform = h2_web_platform_create(&config);
  if (platform == NULL) return 1;
  h2_runtime_config_t rcfg = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  rcfg.log = h2_web_platform_log_api();
  rcfg.task = h2_web_platform_task_api(platform);
  rcfg.sync = h2_web_platform_sync_api(platform);
  rcfg.timer = h2_web_platform_timer_api(platform);
  rcfg.touch = h2_web_platform_touch_api(platform);
  rcfg.audio = h2_web_platform_audio_api(platform);
  rcfg.periph = &periph;
  rcfg.component_mapper = &mapper;
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&rcfg, &runtime);
  if (rc == H2_PAL_OK) rc = h2_runtime_input_start(runtime, NULL);
  if (rc == H2_PAL_OK) {
    input_runtime = runtime;
    stop_at = emscripten_get_now() + 4000.0;
    (void)h2_web_main_call(page_input, (const void*[]){&(int){1}});
    const h2_touch_smoke_config_t app = {.width = 800u,
                                         .height = 480u,
                                         .should_stop = should_stop,
                                         .on_started = touch_ready};
    puts("H2_WEB_APP name=touch stage=running");
    rc = h2_touch_smoke_run(runtime, &app);
    (void)h2_web_main_call(page_input, (const void*[]){&(int){2}});
    input_runtime = NULL;
    if (rc == H2_PAL_OK) rc = input_error;
  }
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=touch result=FAIL rc=%d retained=1\n", cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK) rc = cleanup != H2_PAL_OK ? cleanup : destroyed;
  printf("H2_WEB_APP name=touch result=%s rc=%d fs=0 destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
