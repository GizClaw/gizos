#include "FreeRTOS.h"
#include "h2_bk_platform_core.h"
#include "semphr.h"
#include "task.h"
#include <assert.h>
#include <os/os.h>
#include <stdlib.h>
#include <string.h>

void h2_bk_platform_task_test_reset(void);
static struct {
  int fail_alloc, fail_sem, fail_create, fail_join, creates, frees;
  int sem_created, sem_destroyed, task_deleted, stack_fail;
  uint8_t priority;
  uint32_t stack;
  const char *name;
  const char *resolver_name;
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
  if (s.fail_sem) return NULL;
  ++s.sem_created;
  return (SemaphoreHandle_t)st;
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
int rtos_deinit_semaphore(beken_semaphore_t *sem) {
  assert(*sem); ++s.sem_destroyed; *sem = NULL; return kNoErr;
}
void vTaskSuspend(TaskHandle_t task) { assert(task != NULL); }
void vTaskDelete(TaskHandle_t task) { assert(task == (void *)1); ++s.task_deleted; }
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t core) { (void)core; return NULL; }
int rtos_delay_milliseconds(uint32_t ms) { (void)ms; return kNoErr; }
uint32_t rtos_enter_critical(void) { return 0; }
void rtos_exit_critical(uint32_t level) { (void)level; }
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t depth, void *arg, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *tcb, BaseType_t core) {
  assert(stack && tcb && core == 0);
  beken_thread_t task;
  return create(&task, (uint8_t)(configMAX_PRIORITIES - 1u - priority), name, fn,
                depth * sizeof(StackType_t), arg) == kNoErr ? task : NULL;
}
static void *stack_alloc(void *u, size_t n) { return s.stack_fail ? NULL : alloc(u,n); }
static const h2_pal_mem_vtable_t stack_methods = {.alloc=stack_alloc,.free=release};
static const h2_pal_mem_api_t stack_mem = {.vtable=&stack_methods};
static h2_pal_result_t resolve(void *u, const char *n,
                               h2_bk_task_policy_t *out) {
  assert(u == &s);
  s.resolver_name = n;
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
  if (n && strcmp(n, "resolver-error") == 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (n && strcmp(n, "dynamic-high") == 0) {
    *out = (h2_bk_task_policy_t){.sdk_name = "dynamic-sdk",
                                 .core = 0,
                                 .priority = 8,
                                 .min_stack_size = 12288,
                                 .stack_region = H2_BK_TASK_STACK_DEFAULT};
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
static h2_bk_task_policy_config_t cfg(void) {
  return (h2_bk_task_policy_config_t){
      .resolver = resolve, .resolver_user = &s, .task_allocator = &mem_api};
}
static void entry(void *u) { (void)u; }
static void reset(void) {
  h2_bk_platform_task_test_reset();
  assert(s.sem_created == s.sem_destroyed);
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
  h2_bk_task_policy_config_t c = cfg();
  c.task_allocator = NULL;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg();
  c.resolver = NULL;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg();
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
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) ==
             H2_PAL_ERR_NOT_FOUND &&
         s.creates == 0);
  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "dynamic-high";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_OK);
  assert(strcmp(s.resolver_name, "dynamic-high") == 0 && s.priority == 8 &&
         s.stack == 12288 && strcmp(s.name, "dynamic-sdk") == 0);
  assert(api->vtable->join(NULL, t) == H2_PAL_OK);
  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "invalid";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NOT_FOUND);
  o.name = "resolver-error";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0);
  reset();
  c = cfg();
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
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
#endif
  reset();
  c=cfg(); c.psram_stack_allocator=&stack_mem;
  assert(h2_bk_platform_task_configure(&c)==H2_PAL_OK);
  o=(h2_pal_task_options_t){.name="known",.min_stack_size=16384};
  s.stack_fail=1;
  assert(h2_pal_task_start(api,&o,entry,NULL,&t)==H2_PAL_ERR_NO_MEMORY && t==NULL);
  assert(s.creates==0 && s.sem_created==s.sem_destroyed);
  s.stack_fail=0;
  assert(h2_pal_task_start(api,&o,entry,NULL,&t)==H2_PAL_OK && s.stack==16384);
  h2_bk_platform_resource_stats_t live;
  assert(h2_bk_platform_get_resource_stats(&live)==H2_PAL_OK);
  assert(live.live_tasks==1 && live.task_stack_bytes==16384);
  assert(h2_pal_task_join(api,t)==H2_PAL_OK);
  assert(s.sem_created==s.sem_destroyed && s.task_deleted==1);
  assert(h2_bk_platform_get_resource_stats(&live)==H2_PAL_OK);
  assert(live.live_tasks==0 && live.task_stack_bytes==0);
  o.min_stack_size=16385;
  assert(h2_pal_task_start(api,&o,entry,NULL,&t)==H2_PAL_OK && s.stack==16388);
  assert(h2_pal_task_join(api,t)==H2_PAL_OK);
  o.min_stack_size=(size_t)UINT16_MAX*sizeof(StackType_t)+1u;
  assert(h2_pal_task_start(api,&o,entry,NULL,&t)==H2_PAL_ERR_TASK && t==NULL);
  assert(s.sem_created==s.sem_destroyed);
  return 0;
}
