#ifndef H2_MOSAICO_GIZCLAW_FIXTURE_HTTP_H
#define H2_MOSAICO_GIZCLAW_FIXTURE_HTTP_H
#include "h2_corehttp.h"
#include "h2_runtime.h"
typedef struct h2_mosaico_fixture_http {
  h2_corehttp_t *owner;
  h2_pal_http_api_t api;
} h2_mosaico_fixture_http_t;
/* Empty root keeps the borrowed BSP provider. Nonempty root is private to this
 * E2E Runtime and always uses REQUIRED verification. Owner outlives Runtime. */
int h2_mosaico_fixture_http_prepare(h2_mosaico_fixture_http_t *fixture,
                                  h2_runtime_config_t *config,
                                  const char *root_ca_hex);
/* Only after Runtime init failure or all borrowers have stopped. */
void h2_mosaico_fixture_http_destroy(h2_mosaico_fixture_http_t *fixture);
#endif
