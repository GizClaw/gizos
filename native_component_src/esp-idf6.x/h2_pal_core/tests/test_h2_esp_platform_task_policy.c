#include "h2_test_allocator.h"
#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

void h2_esp_platform_task_test_reset(void);

typedef struct test_state {
  int fail_semaphore;
  int fail_create;
  int fail_join;
  int creates;
  int deletes;
  int entry_calls;
  int gives;
  int takes;
  int suspends;
  int self_deletes;
  int caps_deletes;
  int join_on_give;
  int static_create;
  int static_deletes;
  int external_suspends;
  int running_queries;
  int yields;
  int tcb_live;
  int tcb_allocations;
  int tcb_frees;
  int stack_frees;
  int fail_tcb;
  void *stack;
  TaskFunction_t trampoline;
  void *trampoline_ctx;
  h2_pal_task_t *pal_task;
  uint32_t stack_size;
  uint32_t caps;
  UBaseType_t priority;
  BaseType_t core;
  const char *name;
  const char *resolver_name;
} test_state_t;

static test_state_t s;
static int s_semaphore;
static int s_native_task;
static jmp_buf s_worker_exit;

/* Run the real trampoline until the SDK would remove it from the CPU. These
 * deterministic schedules test PAL ownership, not FreeRTOS's SMP internals. */
static void run_worker(void) {
  if (setjmp(s_worker_exit) == 0) {
    s.trampoline(s.trampoline_ctx);
    assert(0 && "a task trampoline must not return");
  }
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  return s.fail_semaphore ? NULL : &s_semaphore;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore == &s_semaphore);
  assert(s.entry_calls == 1 && s.gives == 0 && s.deletes == 0);
  s.gives++;
  s_semaphore = 1;
  if (s.join_on_give) {
    /* Model a joiner scheduled after the semaphore is published but before
     * give returns. Internal workers must also survive handle release here. */
    assert(h2_pal_task_join(h2_esp_platform_task_api(), s.pal_task) == H2_PAL_OK);
    s.pal_task = NULL;
    if (s.caps != 0u || s.static_create) {
      longjmp(s_worker_exit, 1);
    }
  }
  return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore == &s_semaphore && timeout == portMAX_DELAY);
  if (s.fail_join) {
    return 0;
  }
  if (s.gives == 0) {
    run_worker();
  }
  assert(s_semaphore == 1 && s.entry_calls == 1 && s.takes == 0);
  s_semaphore = 0;
  s.takes++;
  return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
  assert(semaphore == &s_semaphore);
  assert(s.deletes == 0);
  if (!s.fail_create && s.creates != 0) {
    assert(s.takes == 1);
    assert(s.caps == 0u || s.caps_deletes == 1);
    assert(!s.static_create || s.static_deletes == 1);
  }
  s.deletes++;
}
static BaseType_t capture_create(const char *name, uint32_t stack_size,
                                 UBaseType_t priority, TaskHandle_t *out_task,
                                 BaseType_t core, uint32_t caps) {
  s.creates++;
  s.name = name;
  s.stack_size = stack_size;
  s.priority = priority;
  s.core = core;
  s.caps = caps;
  *out_task = &s_native_task;
  return s.fail_create ? 0 : pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
                                   uint32_t stack_size, void *ctx,
                                   UBaseType_t priority, TaskHandle_t *out_task,
                                   BaseType_t core) {
  assert(entry != NULL && ctx != NULL);
  s.trampoline = entry;
  s.trampoline_ctx = ctx;
  return capture_create(name, stack_size, priority, out_task, core, 0u);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
                                           const char *name,
                                           uint32_t stack_size, void *ctx,
                                           UBaseType_t priority,
                                           TaskHandle_t *out_task,
                                           BaseType_t core, uint32_t caps) {
  assert(entry != NULL && ctx != NULL);
  s.trampoline = entry;
  s.trampoline_ctx = ctx;
  return capture_create(name, stack_size, priority, out_task, core, caps);
}
void vTaskDelete(TaskHandle_t task) {
  if (task != NULL) {
    assert(task == &s_native_task && s.static_create && s.external_suspends == 1);
    assert(s.running_queries == 0 && s.tcb_live == 1);
    s.static_deletes++;
    return;
  }
  assert(task == NULL && s.caps == 0u && s.gives == 1);
  s.self_deletes++;
  longjmp(s_worker_exit, 1);
}
void vTaskDeleteWithCaps(TaskHandle_t task) {
  assert(task == &s_native_task && s.caps != 0u);
  assert(s.takes == 1 && s.deletes == 0 && s.caps_deletes == 0);
  s.caps_deletes++;
}
void vTaskSuspend(TaskHandle_t task) {
  if (task != NULL) {
    assert(task == &s_native_task && s.static_create && s.takes == 1);
    s.external_suspends++;
    return;
  }
  assert((s.caps != 0u || s.static_create) && s.gives == 1);
  assert(s.caps_deletes == 0 && s.deletes == 0);
  s.suspends++;
  longjmp(s_worker_exit, 1);
}

void *heap_caps_malloc(size_t size, unsigned int caps) {
  assert(size == sizeof(StaticTask_t));
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (s.fail_tcb) return NULL;
  ++s.tcb_live;
  ++s.tcb_allocations;
  return malloc(size);
}
void heap_caps_free(void *ptr) {
  if (ptr != NULL) {
    assert(s.tcb_live == 1);
    --s.tcb_live;
    ++s.tcb_frees;
    free(ptr);
  }
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t entry, const char *name,
    uint32_t stack_size, void *ctx, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *storage, BaseType_t core) {
  assert(stack != NULL && storage != NULL && s.tcb_live == 1);
  s.static_create = 1;
  s.stack = stack;
  s.trampoline = entry;
  s.trampoline_ctx = ctx;
  TaskHandle_t task;
  return capture_create(name, stack_size, priority, &task, core, 0u) == pdPASS
             ? task : NULL;
}
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t core) {
  if (core == 1 && s.running_queries > 0) {
    --s.running_queries;
    return &s_native_task;
  }
  return NULL;
}
void vTaskDelay(TickType_t ticks) {
  assert(ticks == 0u && s.static_deletes == 0 && s.tcb_live == 1);
  ++s.yields;
}

static h2_pal_result_t resolve(void *user, const char *name,
                               h2_esp_task_policy_t *out) {
  assert(user == &s);
  s.resolver_name = name;
  if (name != NULL && strcmp(name, "known") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = 9u,
        .core = H2_ESP_TASK_CORE_1,
        .min_stack_size = 8192u,
        .stack_region = H2_ESP_TASK_STACK_INTERNAL,
    };
    return H2_PAL_OK;
  }
  if (name != NULL && strcmp(name, "invalid") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = configMAX_PRIORITIES,
        .core = H2_ESP_TASK_CORE_0,
        .min_stack_size = 4096u,
        .stack_region = H2_ESP_TASK_STACK_INTERNAL,
    };
    return H2_PAL_OK;
  }
  if (name != NULL && strcmp(name, "resolver-error") == 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (name != NULL && strcmp(name, "dynamic-high") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = 6u,
        .core = H2_ESP_TASK_CORE_ANY,
        .min_stack_size = 12288u,
        .stack_region = H2_ESP_TASK_STACK_PSRAM,
    };
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_esp_task_policy_config_t config(void) {
  return (h2_esp_task_policy_config_t){
      .resolver = resolve,
      .resolver_user = &s,
  };
}

static void entry(void *user) {
  assert(s.gives == 0 && s.deletes == 0);
  s.entry_calls++;
  if (user != NULL) {
    *(int *)user = 42;
  }
}
static void reset(void) {
  h2_esp_platform_task_test_reset();
  s = (test_state_t){0};
  s_semaphore = 0;
}

static void test_completion(const char *name, int join_on_give) {
  reset();
  h2_esp_task_policy_config_t cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  const h2_pal_task_api_t *api = h2_esp_platform_task_api();
  h2_pal_task_options_t options = {.name = name};
  int result = 0;
  assert(h2_pal_task_start(api, &options, entry, &result, &s.pal_task) == H2_PAL_OK);
  s.join_on_give = join_on_give;
  run_worker();
  assert(result == 42 && s.gives == 1);
  if (!join_on_give) {
    /* Completion alone retains the handle. A failed join must release
     * nothing and the same handle must remain usable for retry. */
    assert(s.deletes == 0 && s.caps_deletes == 0);
    s.fail_join = 1;
    assert(h2_pal_task_join(api, s.pal_task) == H2_PAL_ERR_TASK);
    assert(s.deletes == 0 && s.caps_deletes == 0 && s.takes == 0);
    s.fail_join = 0;
    assert(h2_pal_task_join(api, s.pal_task) == H2_PAL_OK);
    s.pal_task = NULL;
  }
  assert(s.takes == 1 && s.deletes == 1);
  if (s.caps != 0u || s.static_create) {
    assert(s.caps_deletes == 1 && s.self_deletes == 0);
    assert(s.suspends == (join_on_give ? 0 : 1));
  } else {
    assert(s.self_deletes == 1 && s.caps_deletes == 0 && s.suspends == 0);
  }
}

static void stack_free(void *user, void *ptr) {
  if (ptr != NULL) {
    assert(s.fail_create || s.fail_tcb || s.static_deletes == 1);
    assert(s.stack_frees == 0);
    ++s.stack_frees;
  }
  h2_test_free(user, ptr);
}

static void test_psram_stack_allocator(void) {
  /* PSRAM, racing join, internal, SDK failure, TCB failure, stack failure,
   * retryable join, and NULL allocator retaining the WithCaps path. */
  for (int mode = 0; mode < 8; ++mode) {
    reset();
    h2_test_allocator_t arena;
    h2_test_allocator_init(&arena);
    const h2_pal_mem_vtable_t vtable = {
        .alloc = h2_test_alloc, .realloc = h2_test_realloc, .free = stack_free,
    };
    arena.api.vtable = &vtable;
    h2_esp_task_policy_config_t cfg = config();
    cfg.psram_stack_allocator = mode == 7 ? NULL : &arena.api;
    assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
    h2_pal_task_options_t options = {
        .name = mode == 2 ? "known" : "dynamic-high",
    };
    s.fail_create = mode == 3;
    s.fail_tcb = mode == 4;
    if (mode == 5) atomic_store(&arena.fail_on_call, 1u);
    int rc = h2_pal_task_start(h2_esp_platform_task_api(), &options, entry,
                              NULL, &s.pal_task);
    if (mode >= 3 && mode <= 5) {
      assert(rc == (mode == 3 ? H2_PAL_ERR_TASK : H2_PAL_ERR_NO_MEMORY));
      assert(s.pal_task == NULL && s.tcb_live == 0);
      assert(s.creates == (mode == 3 ? 1 : 0));
      assert(s.deletes == 1 && s.entry_calls == 0);
    } else {
      assert(rc == H2_PAL_OK);
      assert(atomic_load(&arena.live) == (mode == 2 || mode == 7 ? 0u : 1u));
      if (mode != 2) assert(s.stack_size == 12288u);
      if (mode == 6) {
        s.fail_join = 1;
        assert(h2_pal_task_join(h2_esp_platform_task_api(), s.pal_task) == H2_PAL_ERR_TASK);
        assert(atomic_load(&arena.live) == 1u && s.tcb_live == 1);
        s.fail_join = 0;
      }
      s.join_on_give = mode == 1;
      s.running_queries = mode == 2 || mode == 7 ? 0 : 2;
      run_worker();
      if (!s.join_on_give)
        assert(h2_pal_task_join(h2_esp_platform_task_api(), s.pal_task) == H2_PAL_OK);
      assert(s.tcb_live == 0);
      if (mode != 2 && mode != 7) {
        assert(s.static_create == 1 && s.static_deletes == 1 && s.yields == 2);
        assert(s.caps_deletes == 0 && s.self_deletes == 0);
      } else {
        assert(s.static_create == 0 && s.static_deletes == 0);
        assert(s.caps_deletes == (mode == 7 ? 1 : 0));
        assert(s.caps == (mode == 7 ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : 0u));
      }
    }
    assert(atomic_load(&arena.live) == 0u);
    assert(atomic_load(&arena.calls) == (mode == 2 || mode == 7 ? 0u : 1u));
    assert(s.stack_frees == (mode == 2 || mode == 5 || mode == 7 ? 0 : 1));
    assert(s.tcb_allocations == (mode == 2 || mode == 4 || mode == 7 ? 0 : 1));
    assert(s.tcb_frees == s.tcb_allocations);
  }
}

int main(void) {
  test_psram_stack_allocator();
  const h2_pal_task_api_t *api = h2_esp_platform_task_api();
  h2_pal_task_t *task = (h2_pal_task_t *)(uintptr_t)7u;
  h2_pal_task_options_t options = {.name = "known", .min_stack_size = 1024u};

  reset();
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(task == NULL);
  h2_esp_task_policy_config_t cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_STATE);

  reset();
  assert(h2_esp_platform_task_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  cfg = config();
  cfg.resolver = NULL;
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_ARG);
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) == H2_PAL_OK);
  assert(task != NULL && s.creates == 1 && s.priority == 9u && s.core == 1 &&
         s.stack_size == 8192u && s.caps == 0u);
  s.fail_join = 1;
  assert(api->vtable->join(NULL, task) == H2_PAL_ERR_TASK);
  assert(s.entry_calls == 0 && s.deletes == 0 && s.caps_deletes == 0);
  s.fail_join = 0;
  assert(api->vtable->join(NULL, task) == H2_PAL_OK);

  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "unknown";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(s.creates == 0 && task == NULL);

  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "dynamic-high";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) == H2_PAL_OK);
  assert(strcmp(s.resolver_name, "dynamic-high") == 0 && s.priority == 6u &&
         s.stack_size == 12288u && s.core == tskNO_AFFINITY &&
         s.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  assert(api->vtable->join(NULL, task) == H2_PAL_OK);

  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "invalid";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0);

  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "unknown";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NOT_FOUND);
  options.name = "resolver-error";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0);

  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  s.fail_semaphore = 1;
  options.name = "known";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NO_MEMORY);
  s.fail_semaphore = 0;
  s.fail_create = 1;
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.deletes == 1);

#if SIZE_MAX > UINT32_MAX
  reset();
  cfg = config();
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0 && task == NULL);
#endif
  test_completion("known", 0);
  test_completion("known", 1);
  test_completion("dynamic-high", 0);
  test_completion("dynamic-high", 1);
  return 0;
}
