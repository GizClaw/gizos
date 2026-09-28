#include "h2_web_platform.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned destroy_calls;

static h2_pal_result_t test_platform_destroy(h2_web_platform_t *platform) {
  if (platform == NULL)
    abort();
  return ++destroy_calls == 1u ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}

// Compile the real close_step with a deterministic platform teardown result.
#define h2_web_platform_destroy test_platform_destroy
#include "../src/h2_h2loader_web.c"
#undef h2_web_platform_destroy

int main(void) {
  h2_h2loader_web_client_t *client = calloc(1u, sizeof(*client));
  if (client == NULL)
    return 1;
  client->json = malloc(1u);
  if (client->json == NULL)
    return 1;
  client->platform = (h2_web_platform_t *)client;
  client->closing = 1;
  client->shutdown_result = H2_PAL_OK;

  if (h2_h2loader_web_close_step(client) != H2_PAL_ERR_WOULD_BLOCK ||
      destroy_calls != 1u || client->json == NULL || !client->closing)
    return 1;
  if (h2_h2loader_web_close_step(client) != H2_PAL_OK ||
      destroy_calls != 2u)
    return 1;
  puts("H2LOADER_CLOSE_BUSY_TEST PASS");
  return 0;
}
