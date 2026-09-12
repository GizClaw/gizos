#include "h2_lua_capability.h"
#include "h2_web_lua_app.h"

#include <stdio.h>

static unsigned s_releases;

static h2_pal_result_t echo(void *user, h2_lua_capability_request_id_t request,
                            const char *input, const char *options,
                            char *output, size_t output_capacity,
                            const char **out_error) {
  (void)user;
  (void)request;
  (void)options;
  (void)out_error;
  (void)snprintf(output, output_capacity, "%s", input);
  return H2_PAL_OK;
}

static h2_pal_result_t register_host(h2_lua_host_t *host) {
  return h2_lua_register_capability(host, "smoke.echo", echo, NULL, NULL);
}

/* Rejects the first exit-Button release and accepts the second, so the
 * browser test proves the hook, not the default, decides cancellation. */
static bool exit_requested(const h2_runtime_event_t *event) {
  if (event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
      event->payload_size < sizeof(h2_runtime_button_action_event_t) ||
      !h2_runtime_button_action_is_released(event->payload))
    return false;
  ++s_releases;
  printf("H2_WEB_LUA_APP_SMOKE exit=%s\n",
         s_releases == 1u ? "rejected" : "accepted");
  return s_releases > 1u;
}

const h2_web_lua_app_extension_t h2_web_lua_app_extension = {
    .register_host = register_host,
    .exit_requested = exit_requested,
};
