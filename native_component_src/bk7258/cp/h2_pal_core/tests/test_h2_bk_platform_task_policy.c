#include "h2_bk_platform_core.h"
#include <assert.h>
#include <os/os.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
void h2_bk_platform_task_test_reset(void);
static struct {
  int fail_alloc, fail_sem, fail_create, fail_join, creates, deinits;
  int entry_calls, gives, takes, frees;
  int self_deletes, handle_deletes, self_suspends, handle_suspends;
  int join_on_give;
  beken_thread_function_t trampoline;
  void *trampoline_ctx;
  h2_pal_task_t *pal_task;
  uint8_t priority;
  uint32_t stack;
  const char *name;
  const char *resolver_name;
} s;
static int s_native_task;
static jmp_buf s_worker_exit;

/* Run the real trampoline until the SDK would take it off the CPU. These
 * deterministic schedules test PAL ownership, not FreeRTOS scheduling. */
static void run_worker(void) {
  if (setjmp(s_worker_exit) == 0) {
    s.trampoline(s.trampoline_ctx);
    assert(0 && "a task trampoline must not return");
  }
}

void *os_malloc(size_t n) { return s.fail_alloc ? NULL : malloc(n); }
void os_free(void *p) {
  /* A reclaimed worker must be deleted before its PAL handle disappears. */
  assert(s.handle_deletes == 0 || s.handle_suspends == 1);
  s.frees++;
  free(p);
}
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
  assert(s.entry_calls == 1 && s.gives == 0);
  assert(s.self_deletes == 0 && s.handle_deletes == 0 && s.deinits == 0);
  s.gives++;
  if (s.join_on_give) {
    /* Model a joiner scheduled the moment completion is published, before the
     * worker reaches its own suspension. The worker must not touch the PAL
     * handle afterwards, whichever stack region it used. */
    assert(h2_pal_task_join(h2_bk_platform_task_api(), s.pal_task) == H2_PAL_OK);
    s.pal_task = NULL;
    if (s.handle_deletes != 0) {
      longjmp(s_worker_exit, 1);
    }
  }
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t *x, uint32_t t) {
  (void)x;
  (void)t;
  if (s.fail_join) {
    return -1;
  }
  if (s.trampoline != NULL && s.gives == 0) {
    run_worker();
  }
  s.takes++;
  return 0;
}
void rtos_deinit_semaphore(beken_semaphore_t *x) {
  (void)x;
  /* Reclamation happens before the completion semaphore is destroyed. */
  assert(s.handle_deletes == 0 || s.handle_suspends == 1);
  s.deinits++;
}
static int create(beken_thread_t *out, uint8_t p, const char *n,
                  beken_thread_function_t e, uint32_t st, void *c) {
  assert(e && c);
  s.creates++;
  s.priority = p;
  s.name = n;
  s.stack = st;
  s.trampoline = e;
  s.trampoline_ctx = c;
  *out = &s_native_task;
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
void rtos_delete_thread(beken_thread_t *x) {
  if (x == NULL) {
    /* Only a default-region worker self-deletes, and only after publishing
     * completion. */
    assert(s.gives == 1 && s.handle_deletes == 0 && s.self_suspends == 0);
    s.self_deletes++;
    longjmp(s_worker_exit, 1);
  }
  /* Join-time reclamation suspends the worker, then deletes the explicit
   * handle before the semaphore and the PAL handle are released. */
  assert(*x == &s_native_task);
  assert(s.takes == 1 && s.handle_suspends == 1);
  assert(s.self_deletes == 0 && s.handle_deletes == 0);
  assert(s.deinits == 0 && s.frees == 0);
  s.handle_deletes++;
}
void rtos_suspend_thread(beken_thread_t *x) {
  if (x == NULL) {
    assert(s.gives == 1 && s.self_deletes == 0);
    s.self_suspends++;
    longjmp(s_worker_exit, 1);
  }
  assert(*x == &s_native_task);
  assert(s.takes == 1 && s.handle_deletes == 0);
  s.handle_suspends++;
}
static h2_pal_result_t resolve(void *u, const char *n,
                               h2_bk_task_policy_t *out) {
  assert(u == &s);
  s.resolver_name = n;
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
  if (n && strcmp(n, "resolver-error") == 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (n && strcmp(n, "dynamic-high") == 0) {
    *out = (h2_bk_task_policy_t){.sdk_name = "dynamic-sdk",
                                 .priority = 8,
                                 .min_stack_size = 12288,
                                 .stack_region = H2_BK_TASK_STACK_DEFAULT};
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
static h2_bk_task_policy_config_t cfg(void) {
  return (h2_bk_task_policy_config_t){.resolver = resolve, .resolver_user = &s};
}
static void entry(void *u) {
  assert(s.gives == 0 && s.frees == 0);
  s.entry_calls++;
  if (u != NULL) {
    *(int *)u = 42;
  }
}

/* Run one worker to completion and join it, covering both stack regions and
 * both orderings of completion against the joiner. */
static void test_completion(const char *name, int psram, int join_on_give) {
  h2_bk_platform_task_test_reset();
  memset(&s, 0, sizeof(s));
  h2_bk_task_policy_config_t c = {
      .resolver = resolve, .resolver_user = &s};
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  const h2_pal_task_api_t *api = h2_bk_platform_task_api();
  h2_pal_task_options_t o = {.name = name, .min_stack_size = 1};
  int result = 0;
  assert(h2_pal_task_start(api, &o, entry, &result, &s.pal_task) == H2_PAL_OK);
  s.join_on_give = join_on_give;
  run_worker();
  assert(result == 42 && s.entry_calls == 1 && s.gives == 1);

  if (!join_on_give) {
    /* Completion alone releases nothing, and a failed join must leave the
     * handle intact and reusable. */
    assert(s.deinits == 0 && s.frees == 0 && s.handle_deletes == 0);
    s.fail_join = 1;
    assert(h2_pal_task_join(api, s.pal_task) == H2_PAL_ERR_TASK);
    assert(s.deinits == 0 && s.frees == 0 && s.takes == 0);
    s.fail_join = 0;
    assert(h2_pal_task_join(api, s.pal_task) == H2_PAL_OK);
    s.pal_task = NULL;
  }

  assert(s.takes == 1 && s.deinits == 1 && s.frees == 1);
  if (psram) {
    /* A PSRAM worker suspends instead of self-deleting, and the joiner frees
     * its stack and TCB rather than waiting for an idle task. */
    assert(s.handle_suspends == 1 && s.handle_deletes == 1);
    assert(s.self_deletes == 0);
    /* Without join_on_give the worker parks itself first; otherwise the joiner
     * deleted it before it ever reached its own suspension. */
    assert(s.self_suspends == (join_on_give ? 0 : 1));
  } else {
    /* A default-region worker keeps idle reclamation, and join must never
     * touch its already released native handle. */
    assert(s.self_deletes == 1);
    assert(s.handle_suspends == 0 && s.handle_deletes == 0);
    assert(s.self_suspends == 0);
  }
}
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
  h2_bk_task_policy_config_t c = cfg();
  c.resolver = NULL;
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_ARG);
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_OK &&
         s.creates == 1 && s.stack == 8192 && strcmp(s.name, "sdk") == 0);
  s.fail_join = 1;
  assert(api->vtable->join(NULL, t) == H2_PAL_ERR_TASK);
  s.fail_join = 0;
  assert(api->vtable->join(NULL, t) == H2_PAL_OK && s.deinits == 1);

  /* PSRAM: the worker parks itself, then the joiner reclaims it. */
  test_completion("known", 1, 0);
  /* PSRAM: the joiner wins the race between completion and self-suspension. */
  test_completion("known", 1, 1);
  /* Default region: self-deletion survives both orderings. */
  test_completion("dynamic-high", 0, 0);
  test_completion("dynamic-high", 0, 1);

  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NOT_FOUND);
  assert(s.creates == 0 && t == NULL);
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
  assert(s.creates == 0 && t == NULL);
  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.name = "missing";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_NOT_FOUND);
  o.name = "resolver-error";
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
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
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK &&
         s.deinits == 1);

#if SIZE_MAX > UINT32_MAX
  reset();
  c = cfg();
  assert(h2_bk_platform_task_configure(&c) == H2_PAL_OK);
  o.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &o, entry, NULL, &t) == H2_PAL_ERR_TASK);
  assert(s.creates == 0 && t == NULL);
#endif
  return 0;
}
