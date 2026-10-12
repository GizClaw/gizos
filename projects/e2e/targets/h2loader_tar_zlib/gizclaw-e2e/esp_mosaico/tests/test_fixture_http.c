#ifdef NDEBUG
#undef NDEBUG
#endif
#include "fixture_http.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocated, freed, created, destroyed;
static int allocation_fail, create_fail;
struct h2_corehttp { int valid; };
static struct h2_corehttp owner;
static void *alloc(void *user, size_t size) {
  (void)user;
  if (allocation_fail) return NULL;
  ++allocated;
  return malloc(size);
}
static void release(void *user, void *p) {
  (void)user;
  if (p) { ++freed; free(p); }
}
h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t *config,
    h2_corehttp_t **out, h2_pal_http_api_t *api) {
  ++created;
  assert(config->tls_verify == H2_PAL_NET_TLS_VERIFY_REQUIRED);
  assert(config->root_ca_pem_len == 3u);
  assert(memcmp(config->root_ca_pem, "A-B", 3u) == 0);
  *out = NULL;
  *api = (h2_pal_http_api_t){0};
  if (create_fail) return H2_PAL_ERR_NO_MEMORY;
  *out = &owner;
  return H2_PAL_OK;
}
void h2_corehttp_destroy(h2_corehttp_t *p) {
  if (p) { assert(p == &owner); ++destroyed; }
}
int main(void) {
  const h2_pal_mem_vtable_t mem_vtable = {.alloc = alloc, .free = release};
  const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  h2_pal_http_api_t borrowed = {0};
  h2_runtime_config_t config = {.mem = &mem, .http = &borrowed};
  h2_mosaico_fixture_http_t fixture = {0};
  assert(h2_mosaico_fixture_http_prepare(&fixture, &config, "") == 0);
  assert(config.http == &borrowed && allocated == 0u && created == 0u);
  for (size_t i = 0; i < 3; ++i) {
    const char *invalid[] = {"0", "xz", "AA"};
    assert(h2_mosaico_fixture_http_prepare(&fixture, &config, invalid[i]) ==
           H2_PAL_ERR_INVALID_ARG);
  }
  assert(config.http == &borrowed && allocated == 0u);
  allocation_fail = 1;
  assert(h2_mosaico_fixture_http_prepare(&fixture, &config, "412d42") ==
         H2_PAL_ERR_NO_MEMORY);
  assert(created == 0u);
  allocation_fail = 0;
  create_fail = 1;
  assert(h2_mosaico_fixture_http_prepare(&fixture, &config, "412d42") ==
         H2_PAL_ERR_NO_MEMORY);
  assert(allocated == freed && fixture.owner == NULL && config.http == &borrowed);
  create_fail = 0;
  assert(h2_mosaico_fixture_http_prepare(&fixture, &config, "412d42") == 0);
  assert(allocated == freed && fixture.owner == &owner && config.http == &fixture.api);
  assert(h2_mosaico_fixture_http_prepare(&fixture, &config, "412d42") ==
         H2_PAL_ERR_INVALID_STATE);
  h2_mosaico_fixture_http_destroy(&fixture);
  assert(destroyed == 1u && fixture.owner == NULL);
  h2_mosaico_fixture_http_destroy(&fixture);
  assert(destroyed == 1u);
  return 0;
}
