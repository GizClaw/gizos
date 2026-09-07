#include "h2_app_test_mem.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef union allocation {
  max_align_t alignment;
  struct {
    size_t size;
    union allocation *previous, *next;
  } value;
} allocation_t;
static void *allocate_block(h2_app_test_mem_t *m, size_t size) {
  if (size > SIZE_MAX - sizeof(allocation_t) || size > SIZE_MAX - m->live_bytes)
    return NULL;
  allocation_t *a = m->delegate
                        ? h2_pal_mem_alloc(m->delegate, sizeof(*a) + size)
                        : malloc(sizeof(*a) + size);
  if (!a)
    return NULL;
  a->value.size = size;
  a->value.previous = NULL;
  a->value.next = m->allocations;
  if (a->value.next)
    a->value.next->value.previous = a;
  m->allocations = a;
  ++m->live_blocks;
  m->live_bytes += size;
  if (m->live_bytes > m->peak_bytes)
    m->peak_bytes = m->live_bytes;
  return a + 1;
}
static void *allocate(void *user, size_t size) {
  h2_app_test_mem_t *m = user;
  if (m->calls != SIZE_MAX)
    ++m->calls;
  if (m->fail_at && m->calls == m->fail_at)
    return NULL;
  return allocate_block(m, size);
}
static void release(void *user, void *ptr) {
  if (!ptr)
    return;
  h2_app_test_mem_t *m = user;
  allocation_t *a = (allocation_t *)ptr - 1;
  if (a->value.previous)
    a->value.previous->value.next = a->value.next;
  else
    m->allocations = a->value.next;
  if (a->value.next)
    a->value.next->value.previous = a->value.previous;
  --m->live_blocks;
  m->live_bytes -= a->value.size;
  if (m->delegate)
    h2_pal_mem_free(m->delegate, a);
  else
    free(a);
}
static void *reallocate(void *user, void *ptr, size_t size) {
  if (!ptr)
    return allocate(user, size);
  if (!size) {
    release(user, ptr);
    return NULL;
  }
  h2_app_test_mem_t *m = user;
  if (m->calls != SIZE_MAX)
    ++m->calls;
  if (m->fail_at && m->calls == m->fail_at)
    return NULL;
  allocation_t *old = (allocation_t *)ptr - 1;
  void *next = allocate_block(m, size);
  if (!next)
    return NULL;
  memcpy(next, ptr, size < old->value.size ? size : old->value.size);
  release(user, ptr);
  return next;
}
static const h2_pal_mem_vtable_t vtable = {
    .alloc = allocate,
    .realloc = reallocate,
    .free = release,
};
void h2_app_test_mem_init(h2_app_test_mem_t *m,
                          const h2_pal_mem_api_t *delegate) {
  if (!m)
    return;
  memset(m, 0, sizeof(*m));
  m->delegate = delegate;
  m->api = (h2_pal_mem_api_t){m, &vtable};
}

void h2_app_test_mem_release_all(h2_app_test_mem_t *m) {
  if (!m)
    return;
  while (m->allocations)
    release(m, (allocation_t *)m->allocations + 1);
}
