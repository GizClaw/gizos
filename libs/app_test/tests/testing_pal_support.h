#ifndef H2_APP_TESTING_PAL_SUPPORT_H
#define H2_APP_TESTING_PAL_SUPPORT_H
#include "h2/pal/os/h2_pal_mem.h"
#include <assert.h>
#include <stdlib.h>
typedef struct test_memory {
  size_t live, calls, fail_at;
} test_memory_t;
static void *test_alloc(void *user, size_t size) {
  test_memory_t *m = user;
  if (++m->calls == m->fail_at)
    return NULL;
  void *p = malloc(size);
  if (p)
    ++m->live;
  return p;
}
static void test_free(void *user, void *p) {
  if (!p)
    return;
  test_memory_t *m = user;
  assert(m->live);
  --m->live;
  free(p);
}
static const h2_pal_mem_vtable_t test_memory_vtable = {.alloc = test_alloc,
                                                       .free = test_free};
#endif
