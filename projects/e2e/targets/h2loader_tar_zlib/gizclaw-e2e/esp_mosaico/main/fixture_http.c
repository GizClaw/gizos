#include "fixture_http.h"
#include <string.h>
static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
int h2_mosaico_fixture_http_prepare(h2_mosaico_fixture_http_t *fixture,
                                  h2_runtime_config_t *config,
                                  const char *hex) {
  if (!fixture || !config || !hex) return H2_PAL_ERR_INVALID_ARG;
  if (fixture->owner) return H2_PAL_ERR_INVALID_STATE;
  size_t length = strlen(hex);
  if (!length) return H2_PAL_OK;
  if (length > 32768u || length % 2u) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < length; ++i)
    if (hex_digit(hex[i]) < 0) return H2_PAL_ERR_INVALID_ARG;
  uint8_t *pem = h2_pal_mem_alloc(config->mem, length / 2u);
  if (!pem) return H2_PAL_ERR_NO_MEMORY;
  for (size_t i = 0u; i < length; i += 2u)
    pem[i / 2u] = (uint8_t)((hex_digit(hex[i]) << 4u) | hex_digit(hex[i + 1u]));
  const h2_corehttp_config_t http = {
      .allocator = config->mem, .net = config->net, .time = config->time,
      .log = config->log, .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .root_ca_pem = pem, .root_ca_pem_len = length / 2u};
  int rc = h2_corehttp_create(&http, &fixture->owner, &fixture->api);
  h2_pal_mem_free(config->mem, pem);
  if (rc == H2_PAL_OK) config->http = &fixture->api;
  return rc;
}
void h2_mosaico_fixture_http_destroy(h2_mosaico_fixture_http_t *fixture) {
  if (!fixture) return;
  h2_corehttp_destroy(fixture->owner);
  fixture->owner = NULL;
  fixture->api = (h2_pal_http_api_t){0};
}
