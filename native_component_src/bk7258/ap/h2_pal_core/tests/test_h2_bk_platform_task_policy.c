#include "FreeRTOS.h"
#include "h2_bk_platform_core.h"
#include "semphr.h"
#include <assert.h>
#include <os/os.h>
#include <stdlib.h>
#include <string.h>

void h2_bk_platform_task_test_reset(void);
static struct {
  int fail_alloc, fail_sem, fail_create, fail_join, creates, frees;
  uint8_t priority;
  uint32_t stack;
  const char *name;
  const char *fallback_name;
} s;
static void *alloc(void *u, size_t n) {
  (void)u;
  return s.fail_alloc ? NULL : malloc(n);
}
static void release(void *u, void *p) {
  (void)u;
  s.frees++;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = alloc, .free = release};
static const h2_pal_mem_api_t mem_api = {.vtable = &mem_vtable};
void *os_memset(void *p, int v, size_t n) { return memset(p, v, n); }
void *os_malloc(size_t n) { return malloc(n); }
void os_free(void *p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *st) {
  return s.fail_sem ? NULL : (SemaphoreHandle_t)st;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t x) {
  (void)x;
  return pdPASS;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t x, uint32_t t) {
  (void)x;
  (void)t;
  return s.fail_join ? 0 : pdPASS;
}
static int create(beken_thread_t *out, uint8_t pr, const char *name,
                  beken_thread_function_t e, uint32_t stack, void *ctx) {
  assert(e && ctx);
  s.creates++;
  s.priority = pr;
  s.name = name;
  s.stack = stack;
  *out = (void *)1;
  return s.fail_create ? -1 : kNoErr;
}
int rtos_create_thread(beken_thread_t *a, uint8_t b, const char *c,
                       beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_create_psram_thread(beken_thread_t *a, uint8_t b, const char *c,
                             beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_core0_create_thread(beken_thread_t *a, uint8_t b, const char *c,
                             beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_core0_create_psram_thread(beken_thread_t *a, uint8_t b, const char *c,
                                   beken_thread_function_t d, uint32_t e,
                                   void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_core1_create_thread(beken_thread_t *a, uint8_t b, const char *c,
                             beken_thread_function_t d, uint32_t e, void *f) {
  return create(a, b, c, d, e, f);
}
int rtos_core1_create_psram_thread(beken_thread_t *a, uint8_t b, const char *c,
                                   beken_thread_function_t d, uint32_t e,
                                   void *f) {
  return create(a, b, c, d, e, f);
}
void rtos_delete_thread(beken_thread_t *x) { assert(x == NULL); }
static h2_pal_result_t resolve(void *u, const char *n,
                               h2_bk_task_policy_t *out) {
  (void)u;
  if (n && strcmp(n, "known") == 0) {
    *out = (h2_bk_task_policy_t){.sdk_name = "sdk",
                                 .core = 0,
                                 .priority = 5,
                                 .min_stack_size = 8192,
                                 .stack_region = H2_BK_TASK_STACK_PSRAM};
    return H2_PAL_OK;
  }
  if (n && strcmp(n, "invalid") == 0) {
    *out = (h2_bk_task_policy_t){.core = 2,
                                 .priority = 5,
                                 .min_stack_size = 4096,
                                 .stack_region = H2_BK_TASK_STACK_DEFAULT};
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
static h2_pal_result_t resolve_fallback(void *u, const char *n,
                                        h2_bk_task_policy_t *out) {
  assert(u == &s);
  s.fallback_name = n;
  if (n && strcmp(n, "fallback-reject") == 0)
    return H2_PAL_ERR_NOT_FOUND;
  if (n && strcmp(n, "fallback-error") == 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (n && strcmp(n, "fallback-invalid") == 0) {
    *out = (h2_bk_task_policy_t){.core = 2,
                                 .priority = 7,
                                 .min_stack_size = 4096,
                                 .stack_region = H2_BK_TASK_STACK_DEFAULT};
    return H2_PAL_OK;
  }
  *out = (h2_bk_task_policy_t){
      .sdk_name = n && strcmp(n, "dynamic-high") == 0 ? "dynamic-sdk" : NULL,
      .core = 0,
      .priority = n && strcmp(n, "dynamic-high") == 0 ? 8 : 7,
      .min_stack_size = n && strcmp(n, "dynamic-high") == 0 ? 12288 : 4096,
      .stack_region = H2_BK_TASK_STACK_DEFAULT};
  return H2_PAL_OK;
}
static h2_bk_task_policy_config_t
cfg(h2_bk_task_policy_resolver_t fallback_resolver) {
  return (h2_bk_task_policy_config_t){.resolver = resolve,
                                      .fallback_resolver = fallback_resolver,
                                      .resolver_user = &s,
                                      .task_allocator = &mem_api};
}
static void entry(void *u) { (void)u; }
static void reset(void) {
  h2_bk_platform_task_test_reset();
  memset(&s, 0, sizeof(s));
}
int main(void) {
  const h2_pal_task_api_t *api = h2_bk_platform_task_api();
  h2_pal_task_options_t o = {.name = "known", .min_stack_size = 1};
  h2_pal_task_t *t = (void *)1;
  reset();
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) ==
             H2_PAL_ERR_INVALID_STATE &&
         t == NULL);
  reset();
  h2_bk_task_policy_config_t c = cfg(resolve_fallback);
  c.task_allocator = NULL;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg(resolve_fallback);
  c.resolver = NULL;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_OK);
  assert(s.creates == 1 && s.priority == 5 && s.stack == 8192 &&
         strcmp(s.name, "sdk") == 0);
  s.fail_join = 1;
  assert(api->vtable->join(NULL, t) == H2_PAL_ERR_TASK);
  s.fail_join = 0;
  assert(api->vtable->join(NULL, t) == H2_PAL_OK && s.frees == 1);
  reset();
  c = cfg(NULL);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) ==
             H2_PAL_ERR_NOT_FOUND &&
         s.creates == 0);
  reset();
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "dynamic-high";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_OK);
  assert(strcmp(s.fallback_name, "dynamic-high") == 0 && s.priority == 8 &&
         s.stack == 12288 && strcmp(s.name, "dynamic-sdk") == 0);
  assert(api->vtable->join(NULL, t) == H2_PAL_OK);
  reset();
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "invalid";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  reset();
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "fallback-invalid";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  o.name = "fallback-reject";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NOT_FOUND);
  o.name = "fallback-error";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0);
  reset();
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  s.fail_alloc = 1;
  o.name = "known";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NO_MEMORY);
  s.fail_alloc = 0;
  s.fail_sem = 1;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NO_MEMORY);
  s.fail_sem = 0;
  s.fail_create = 1;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);

#if SIZE_MAX > UINT32_MAX
  reset();
  c = cfg(resolve_fallback);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
#endif
  return 0;
}
