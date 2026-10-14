#include "h2_lua_flappybird.h"
#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include <emscripten.h>

/* clang-format off */
EM_JS(void, web_prepare_headless_canvas,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], null,
    () => {
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
});
/* clang-format on */

/* clang-format off */
EM_JS(void, web_set_status,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer"], null,
    (status) => {
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
});
/* clang-format on */

/* clang-format off */
EM_JS(void, web_is_headless,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.document ? 0 : 1; });
});
/* clang-format on */

static const h2_web_application_host_button_t buttons[] = {
    {H2_LUA_FLAPPYBIRD_COMPONENT_BACK, "Escape", "back"}};
static h2_pal_result_t ready(void *user) {
  h2_pal_result_t rc = h2_application_host_ready(user);
  (void)h2_web_main_call(web_set_status, (const void *[]){&(const char *){"Running Lua Flappy Bird"}});
  if (h2_web_main_call(web_is_headless, NULL).i32) h2_application_host_request_stop(user);
  return rc;
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_lua_flappybird_config_t config = {
      .button_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .back_component_id = H2_LUA_FLAPPYBIRD_COMPONENT_BACK,
      .should_stop = h2_application_host_should_stop, .should_stop_user = host,
      .on_ready = ready, .on_ready_user = host};
  int rc = h2_lua_flappybird_run(runtime, &config);
  (void)h2_web_main_call(web_set_status, (const void *[]){&(const char *){rc == H2_PAL_OK ? "Stopped" : "Lua App failed"}});
  return rc;
}
int main(void) {
  (void)h2_web_main_call(web_prepare_headless_canvas, NULL);
  const h2_web_application_host_config_t config = {
      .name = "lua-flappybird", .display_width = 368, .display_height = 448,
      .buttons = buttons, .button_count = 1u, .stack_size = 262144u};
  int rc = h2_web_application_host_run(&config, run, NULL);
  if (h2_web_main_call(web_is_headless, NULL).i32) emscripten_force_exit(rc);
  return rc;
}
