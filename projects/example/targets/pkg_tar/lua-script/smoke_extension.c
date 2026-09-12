#include "h2_lua_capability.h"
#include "h2_web_lua_app.h"

#include <stdio.h>

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

const h2_web_lua_app_extension_t h2_web_lua_app_extension = {
    .register_host = register_host,
};
