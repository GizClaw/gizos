#include "h2_lua_flappybird.h"
#include "h2_lua_flappybird_task_names.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>

static h2_runtime_t *s_runtime;

/* clang-format off */
EM_JS(void, web_prepare_headless_canvas, (), {
  if (globalThis.document || Module['canvas'])
    return;
  globalThis.ImageData ||= class {
    constructor(data, width, height) {
      this.data = data;
      this.width = width;
      this.height = height;
    }
  };
  Module['canvas'] = {
    style: {},
    getContext: () => ({putImageData: () => {}}),
    getBoundingClientRect: () => ({left: 0, top: 0, width: 368, height: 448}),
    addEventListener: () => {},
    removeEventListener: () => {},
    setPointerCapture: () => {},
  };
});

EM_JS(void, web_set_status, (const char *status), {
  const text = UTF8ToString(status);
  const document = globalThis.document;
  if (!document) {
    console.log(`H2_LUA_FLAPPYBIRD status=${text}`);
    return;
  }
  const element = document.getElementById('status');
  if (element)
    element.textContent = text;
});

EM_JS(int, web_is_headless, (), { return globalThis.document ? 0 : 1; });
/* clang-format on */

static const h2_pal_periph_single_button_payload_t s_back_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static h2_pal_result_t
list_components(void *user, h2_runtime_component_t filter,
                h2_runtime_component_mapping_cb_t callback,
                void *callback_user) {
  h2_runtime_component_mapping_entry_t entry;
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  entry = (h2_runtime_component_mapping_entry_t){
      H2_LUA_FLAPPYBIRD_COMPONENT_BACK, 1u};
  return callback(callback_user, &entry);
}

static h2_pal_result_t get_periph_id(void *user,
                                     h2_runtime_component_id_t component_id,
                                     h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id != H2_LUA_FLAPPYBIRD_COMPONENT_BACK)
    return H2_PAL_ERR_NOT_FOUND;
  *out_periph_id = 1u;
  return H2_PAL_OK;
}

static h2_pal_result_t list_peripherals(void *user, h2_pal_periph_type_t filter,
                                        h2_pal_periph_cb_t callback,
                                        void *callback_user) {
  const h2_pal_periph_info_t info = {
      .id = 1u,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .name = "back",
      .payload = &s_back_payload,
      .payload_size = sizeof(s_back_payload),
  };
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  return callback(callback_user, &info);
}

static h2_pal_result_t get_peripheral(void *user, h2_pal_periph_id_t id,
                                      h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id != 1u)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = (h2_pal_periph_info_t){
      .id = 1u,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .name = "back",
      .payload = &s_back_payload,
      .payload_size = sizeof(s_back_payload),
  };
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = list_components,
    .get_periph_id = get_periph_id,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};

static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = list_peripherals,
    .get = get_peripheral,
};

static const h2_pal_periph_api_t s_periph = {
    .vtable = &s_periph_vtable,
};

EMSCRIPTEN_KEEPALIVE int h2_web_flappybird_back(void) {
  h2_pal_result_t result;
  if (s_runtime == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  result =
      h2_runtime_button_push_edge(s_runtime, 1u, H2_RUNTIME_BUTTON_EDGE_DOWN);
  if (result == H2_PAL_OK) {
    result =
        h2_runtime_button_push_edge(s_runtime, 1u, H2_RUNTIME_BUTTON_EDGE_UP);
  }
  return result;
}

static int never_stop(void *user) {
  (void)user;
  return 0;
}

static h2_pal_result_t stop_headless_app(void *user) {
  (void)user;
  return web_is_headless() ? h2_web_flappybird_back() : H2_PAL_OK;
}

typedef struct web_app_context {
  h2_web_platform_t *platform;
  h2_pal_result_t result;
  volatile int done;
} web_app_context_t;

static void run_app(void *user) {
  web_app_context_t *context = user;
  h2_runtime_t *runtime = NULL;
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(context->platform),
      h2_web_platform_queue_api(context->platform),
      h2_web_platform_display_api(context->platform));
  config.log = h2_web_platform_log_api();
  config.timer = h2_web_platform_timer_api(context->platform);
  config.task = h2_web_platform_task_api(context->platform);
  config.sync = h2_web_platform_sync_api(context->platform);
  config.touch = h2_web_platform_touch_api(context->platform);
  config.webrtc = h2_web_platform_webrtc_api(context->platform);
  config.periph = &s_periph;
  config.component_mapper = &s_mapper;
  context->result = h2_runtime_init(&config, &runtime);
  if (context->result != H2_PAL_OK) {
    printf("H2_LUA_FLAPPYBIRD runtime_init=%d\n", context->result);
    web_set_status("Runtime initialization failed");
    context->done = 1;
    return;
  }
  context->result = h2_runtime_input_start(runtime, NULL);
  if (context->result != H2_PAL_OK) {
    printf("H2_LUA_FLAPPYBIRD runtime_input=%d\n", context->result);
    web_set_status("Runtime input start failed");
    h2_runtime_deinit(runtime);
    context->done = 1;
    return;
  }
  h2_web_platform_install_pointer(context->platform);
  s_runtime = runtime;
  web_set_status("Running Lua Flappy Bird");
  const h2_lua_flappybird_config_t app_config = {
      .button_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .back_component_id = H2_LUA_FLAPPYBIRD_COMPONENT_BACK,
      .should_stop = never_stop,
      .should_stop_user = NULL,
      .on_ready = stop_headless_app,
      .on_ready_user = NULL,
  };
  context->result = h2_lua_flappybird_run(runtime, &app_config);
  s_runtime = NULL;
  h2_runtime_deinit(runtime);
  web_set_status(context->result == H2_PAL_OK ? "Stopped" : "Lua App failed");
  context->done = 1;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = 368,
      .display_height = 448,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  web_app_context_t context = {
      .platform = platform,
  };
  h2_pal_task_t *app_task = NULL;
  h2_pal_result_t result;
  web_prepare_headless_canvas();
  if (platform == NULL) {
    web_set_status("Host allocation failed");
    return 1;
  }
  const h2_pal_task_options_t task_options = {
      .name = h2_lua_flappybird_runner_task_name,
  };
  result = h2_pal_task_start(h2_web_platform_task_api(platform), &task_options,
                             run_app, &context, &app_task);
  while (result == H2_PAL_OK && !context.done) {
    result = h2_web_platform_pump(platform, 64u, NULL);
    if (result == H2_PAL_OK)
      emscripten_sleep(1u);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_task_join(h2_web_platform_task_api(platform), app_task);
  }
  if (result == H2_PAL_OK)
    result = context.result;
  h2_web_platform_destroy(platform);
  return result == H2_PAL_OK ? 0 : 1;
}
