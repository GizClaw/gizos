#include "h2_bk_platform_core.h"
#include <assert.h>
#include <os/os.h>
#include <stdlib.h>
#include <string.h>
void h2_bk_platform_task_test_reset(void);
static struct {
  int fail_alloc, fail_sem, fail_create, fail_join, creates, deinits;
  uint8_t priority;
  uint32_t stack;
  const char *name;
} s;
void *os_malloc(size_t n) { return s.fail_alloc ? NULL : malloc(n); }
void os_free(void *p) { free(p); }
void *os_memset(void *p, int v, size_t n) { return memset(p, v, n); }
int rtos_init_semaphore(beken_semaphore_t *x, int n) {
  (void)n;
  if (s.fail_sem)
    return -1;
  *x = (void *)1;
  return 0;
}
int rtos_set_semaphore(beken_semaphore_t *x) {
  (void)x;
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t *x, uint32_t t) {
  (void)x;
  (void)t;
  return s.fail_join ? -1 : 0;
}
void rtos_deinit_semaphore(beken_semaphore_t *x) {
  (void)x;
  s.deinits++;
}
static int create(beken_thread_t *out, uint8_t p, const char *n,
                  beken_thread_function_t e, uint32_t st, void *c) {
  assert(e && c);
  s.creates++;
  s.priority = p;
  s.name = n;
  s.stack = st;
  *out = (void *)1;
  return s.fail_create ? -1 : 0;
}
int rtos_create_thread(beken_thread_t *a, uint8_t b, const char *c,
                       beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_create_psram_thread(beken_thread_t *a, uint8_t b, const char *c,
                             beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
void rtos_delete_thread(beken_thread_t *x) { assert(x == NULL); }
static h2_pal_result_t resolve(void *u, const char *n,
                               h2_bk_task_policy_t *out) {
  (void)u;
  if (n && strcmp(n, "known") == 0) {
    *out = (h2_bk_task_policy_t){.sdk_name = "sdk",
                                 .priority = 6,
                                 .min_stack_size = 8192,
                                 .stack_region = H2_BK_TASK_STACK_PSRAM};
    return H2_PAL_OK;
  }
  if (n && strcmp(n, "invalid") == 0) {
    *out = (h2_bk_task_policy_t){.priority = UINT8_MAX + 1u,
                                 .min_stack_size = 4096,
                                 .stack_region = H2_BK_TASK_STACK_DEFAULT};
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
static h2_bk_task_policy_config_t cfg(h2_bk_task_unknown_mode_t m) {
  return (h2_bk_task_policy_config_t){
      .resolver = resolve,
      .unknown_mode = m,
      .fallback = {.priority = 6,
                   .min_stack_size = 4096,
                   .stack_region = H2_BK_TASK_STACK_DEFAULT}};
}
static void entry(void *u) { (void)u; }
static void reset(void) {
  h2_bk_platform_task_test_reset();
  memset(&s, 0, sizeof(s));
}
int main(void) {
  const h2_pal_task_api_t *api = h2_bk_platform_task_api();
  h2_pal_task_options_t o = {.name = "known", .min_stack_size = 1};
  h2_pal_task_t *t = NULL;
  reset();
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) ==
         H2_PAL_ERR_INVALID_STATE);
  reset();
  h2_bk_task_policy_config_t c = cfg(H2_BK_TASK_UNKNOWN_FALLBACK);
  c.fallback.min_stack_size = 0;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg((h2_bk_task_unknown_mode_t)99);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg(H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_OK &&
         s.creates == 1 && s.stack == 8192 && strcmp(s.name, "sdk") == 0);
  s.fail_join = 1;
  assert(api->vtable->join(NULL, t) == H2_PAL_ERR_TASK);
  s.fail_join = 0;
  assert(api->vtable->join(NULL, t) == H2_PAL_OK && s.deinits == 1);
  reset();
  c = cfg(H2_BK_TASK_UNKNOWN_REJECT);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NOT_FOUND);
  assert(s.creates == 0 && t == NULL);
  reset();
  c = cfg(H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "invalid";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
  reset();
  c = cfg(H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  s.fail_alloc = 1;
  o.name = "known";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NO_MEMORY);
  s.fail_alloc = 0;
  s.fail_sem = 1;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NO_MEMORY);
  s.fail_sem = 0;
  s.fail_create = 1;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK &&
         s.deinits == 1);

#if SIZE_MAX > UINT32_MAX
  reset();
  c = cfg(H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
#endif
  return 0;
}
