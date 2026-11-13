#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_fruit_slice_desktop_pref.h"
#include "h2_fruit_slice.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  assert(temporary && temporary[0] == '/');
  char root[512], database[512], parent[512];
  assert(snprintf(parent, sizeof(parent), "%s/nested", temporary) > 0);
  assert(snprintf(root, sizeof(root), "%s/game", parent) > 0);
  assert(snprintf(database, sizeof(database), "%s/preferences.sqlite", root) >
         0);
  h2_sqlite_t* provider = NULL;
  assert(h2_fruit_slice_desktop_pref_create(root, &provider) == H2_PAL_OK);
  h2_pal_pref_namespace_t* ns = NULL;
  assert(h2_pal_pref_open(h2_sqlite_pref_api(provider),
                          H2_FRUIT_SLICE_PREF_NAMESPACE,
                          H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  uint32_t best = 0;
  assert(ns->get_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, &best) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(ns->close(ns) == H2_PAL_OK);
  assert(h2_pal_pref_open(h2_sqlite_pref_api(provider),
                          H2_FRUIT_SLICE_PREF_NAMESPACE,
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
  assert(ns->set_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, 27) == H2_PAL_OK);
  assert(ns->commit(ns) == H2_PAL_OK);
  assert(ns->close(ns) == H2_PAL_OK);
  h2_sqlite_destroy(provider);
  provider = NULL;
  /* A fresh provider must recover committed data without resetting the file. */
  assert(h2_fruit_slice_desktop_pref_create(root, &provider) == H2_PAL_OK);
  assert(h2_pal_pref_open(h2_sqlite_pref_api(provider),
                          H2_FRUIT_SLICE_PREF_NAMESPACE,
                          H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  assert(ns->get_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, &best) == H2_PAL_OK &&
         best == 27);
  assert(ns->close(ns) == H2_PAL_OK);
  h2_sqlite_destroy(provider);
  provider = NULL;
  /* Reject invalid roots and a file where an application directory is needed.
   */
  assert(h2_fruit_slice_desktop_pref_create("relative/path", &provider) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(!provider);
  assert(h2_fruit_slice_desktop_pref_create(database, &provider) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(!provider);
  char too_long[5000];
  memset(too_long, 'a', sizeof(too_long));
  too_long[0] = '/';
  too_long[sizeof(too_long) - 1] = '\0';
  assert(h2_fruit_slice_desktop_pref_create(too_long, &provider) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(!provider);
  assert(unlink(database) == 0);
  assert(rmdir(root) == 0);
  assert(rmdir(parent) == 0);
  return 0;
}
