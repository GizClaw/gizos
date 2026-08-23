#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_sqlite.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_crud_and_reopen(void) {
  char path[] = "/tmp/h2-sqlite-test-XXXXXX";
  const int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  assert(close(descriptor) == 0);
  assert(unlink(path) == 0);

  h2_sqlite_t *provider = NULL;
  const h2_sqlite_config_t config = {.path = path};
  assert(h2_sqlite_create(&config, &provider) == H2_PAL_OK);
  assert(provider != NULL);

  h2_pal_pref_namespace_t *store = NULL;
  assert(h2_pal_pref_open(h2_sqlite_pref_api(provider), "test",
                          H2_PAL_PREF_OPEN_READ_WRITE,
                          &store) == H2_PAL_OK);
  assert(store != NULL);
  assert(store->set_u32(store, "counter", 42u) == H2_PAL_OK);
  assert(store->set_string(store, "name", "provider") == H2_PAL_OK);
  assert(store->commit(store) == H2_PAL_OK);
  assert(store->close(store) == H2_PAL_OK);

  assert(h2_pal_pref_open(h2_sqlite_pref_api(provider), "test",
                          H2_PAL_PREF_OPEN_READ_ONLY,
                          &store) == H2_PAL_OK);
  uint32_t counter = 0u;
  assert(store->get_u32(store, "counter", &counter) == H2_PAL_OK);
  assert(counter == 42u);
  assert(store->close(store) == H2_PAL_OK);

  h2_sqlite_destroy(provider);
  assert(unlink(path) == 0);
}

static void test_invalid_configuration(void) {
  h2_sqlite_t *provider = (h2_sqlite_t *)(uintptr_t)1u;
  const h2_sqlite_config_t empty = {.path = ""};
  assert(h2_sqlite_create(NULL, &provider) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_sqlite_create(&empty, &provider) == H2_PAL_ERR_INVALID_ARG);
  assert(provider == NULL);
  assert(h2_sqlite_pref_api(NULL) == NULL);
  h2_sqlite_destroy(NULL);
}

int main(void) {
  test_crud_and_reopen();
  test_invalid_configuration();
  return 0;
}
