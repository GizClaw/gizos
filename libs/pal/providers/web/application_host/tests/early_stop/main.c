#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include <assert.h>
#include <emscripten.h>
#include <stdio.h>

/* clang-format off */
EM_JS(void, stop_before_create,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], null,
    () => Module._h2_web_application_host_request_stop());
});
/* clang-format on */

static h2_pal_result_t
create_after_stop(const h2_application_host_config_t *config,
                  h2_application_host_t **out) {
    // The Web owner is published, but the shared Host does not exist yet.
    // Exercise the real UI export before publishing the shared pointer.
    (void)h2_web_main_call(stop_before_create, NULL);
    return h2_application_host_create(config, out);
}
#define h2_application_host_create create_after_stop
#include "libs/pal/providers/web/application_host/src/h2_web_application_host.c"
#undef h2_application_host_create

static h2_pal_result_t entry(h2_application_host_t *host, h2_runtime_t *runtime,
                             void *user) {
    (void)runtime;
    (void)user;
    assert(h2_application_host_should_stop(host));
    puts("EARLY_STOP request_preserved=PASS");
    return H2_PAL_OK;
}
int main(void) {
    const h2_web_application_host_config_t config = {.name = "early-stop"};
    assert(h2_web_application_host_run(&config, entry, NULL) == 0);
    return 0;
}
