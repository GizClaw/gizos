#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include <assert.h>
#include <emscripten.h>
#include <stdio.h>

/* clang-format off */
EM_JS(void, mode_js, (void *c, h2_web_main_result_t *r, h2_web_main_completion_t *done), {
  h2WebMain(c, r, done, [], 'i32', () => Number(new URLSearchParams(location.search).get('mode') || 0));
});
/* clang-format on */

typedef struct context {
  int mode, before, configured, validated, entered, released;
} context_t;
static context_t context;
static h2_pal_result_t before(void *user) {
  assert(user == &context);
  ++context.before;
  return context.mode == 1 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  assert(user == &context && context.before == 1 && config->task &&
         config->sync);
  ++context.configured;
  return context.mode == 2 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  assert(user == &context && context.configured == 1 && runtime->mem);
  ++context.validated;
  return context.mode == 3 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t entry(h2_application_host_t *host, h2_runtime_t *runtime,
                             void *user) {
  assert(user == &context && context.validated == 1 && runtime->crypto);
  assert(h2_web_application_host_json(host));
  ++context.entered;
  return context.mode == 5 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t release_runtime(void *user) {
  assert(user == &context && context.configured == 1);
  ++context.released;
  return context.mode == 4 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int main(void) {
  const int mode = h2_web_main_call(mode_js, NULL).i32;
  assert(mode >= 0 && mode <= 5);
  const h2_web_application_host_config_t platform = {
      .name = "app-hooks", .display_width = 1, .display_height = 1, .json = 1};
  const h2_application_host_config_t app = {.entry = entry,
                                            .user = &context,
                                            .configure = configure,
                                            .validate = validate,
                                            .release_runtime = release_runtime,
                                            .before_prepare = before};
  h2_application_host_result_t result = {0};
  context.mode = mode;
  int rc = h2_web_application_host_run_app(&platform, &app, &result);
  assert(context.before == 1);
  assert(context.configured == (mode != 1));
  assert(context.validated == (mode != 1 && mode != 2));
  assert(context.entered == (mode == 0 || mode == 4 || mode == 5));
  assert(context.released == (mode != 1));
  if (mode == 4) {
    assert(rc == H2_WEB_APPLICATION_HOST_RETAINED && result.retained);
    assert(result.cleanup == H2_PAL_ERR_IO);
    assert(h2_web_application_host_run_app(&platform, &app, &result) ==
           H2_WEB_APPLICATION_HOST_RETAINED);
    assert(result.retained);
  } else {
    assert(rc == (mode == 0 ? 0 : 1) && !result.retained &&
           result.cleanup == H2_PAL_OK);
    assert(result.application == (mode == 0 ? H2_PAL_OK : H2_PAL_ERR_IO));
    context = (context_t){0};
    assert(h2_web_application_host_run_app(&platform, &app, &result) == 0);
    assert(!result.retained && result.cleanup == H2_PAL_OK);
  }
  printf("WEB_APP_HOOKS PASS mode=%d\n", mode);
  return 0;
}
