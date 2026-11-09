#include "h2_esp_platform_entry_task.h"
#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  h2_pal_result_t reserve_result;
  h2_esp_task_policy_t policy;
  int fail_alloc;
  int fail_create;
  int reserves;
  int allocations;
  int frees;
  int starts;
  int entries;
  int cleanups;
  int deletes;
  unsigned int context_caps;
  unsigned int stack_caps;
  uint32_t stack_size;
  UBaseType_t priority;
  BaseType_t core;
  TaskFunction_t trampoline;
  void* context;
} test_state_t;

static test_state_t s;

h2_pal_result_t h2_esp_platform_task_reserve_policy(const char* name,
                                                    h2_esp_task_policy_t* out) {
  assert(strcmp(name, "entry/test") == 0 && out != NULL);
  ++s.reserves;
  *out = s.policy;
  return s.reserve_result;
}

void* heap_caps_malloc(size_t size, unsigned int caps) {
  ++s.allocations;
  s.context_caps = caps;
  return s.fail_alloc ? NULL : malloc(size);
}
void heap_caps_free(void* ptr) {
  assert(ptr != NULL && s.frees == 0);
  ++s.frees;
  free(ptr);
}

static BaseType_t capture_create(TaskFunction_t entry, const char* name,
                                 uint32_t stack_size, void* context,
                                 UBaseType_t priority, TaskHandle_t* out_task,
                                 BaseType_t core, unsigned int caps) {
  assert(strcmp(name, "entry/test") == 0 && out_task == NULL);
  assert(entry != NULL && context != NULL && s.frees == 0);
  ++s.starts;
  s.trampoline = entry;
  s.context = context;
  s.stack_size = stack_size;
  s.priority = priority;
  s.core = core;
  s.stack_caps = caps;
  return s.fail_create ? 0 : pdPASS;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
                                           const char* name,
                                           uint32_t stack_size, void* context,
                                           UBaseType_t priority,
                                           TaskHandle_t* out_task,
                                           BaseType_t core, uint32_t caps) {
  return capture_create(entry, name, stack_size, context, priority, out_task,
                        core, caps);
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char* name,
                                   uint32_t stack_size, void* context,
                                   UBaseType_t priority, TaskHandle_t* out_task,
                                   BaseType_t core) {
  return capture_create(entry, name, stack_size, context, priority, out_task,
                        core, 0u);
}
void vTaskDeleteWithCaps(TaskHandle_t task) {
  assert(task == NULL && s.policy.stack_region == H2_ESP_TASK_STACK_PSRAM);
  assert(s.entries == 1 && s.frees == 1 && s.deletes == 0);
  ++s.deletes;
}
void vTaskDelete(TaskHandle_t task) {
  assert(task == NULL && s.policy.stack_region == H2_ESP_TASK_STACK_INTERNAL);
  assert(s.entries == 1 && s.frees == 1 && s.deletes == 0);
  ++s.deletes;
}

static void entry(void* user) {
  assert(user == &s && s.frees == 1 && s.cleanups == 0 && s.deletes == 0);
  ++s.entries;
}
static void cleanup(void* user) {
  assert(user == &s.cleanups && s.entries == 1 && s.deletes == 0);
  ++s.cleanups;
}
static h2_pal_result_t start(h2_esp_platform_entry_task_fn after_entry) {
  return h2_esp_platform_start_entry_task("entry/test", entry, &s, after_entry,
                                          &s.cleanups);
}
static void reset(void) {
  s = (test_state_t){.policy = {
                         .priority = 4u,
                         .core = H2_ESP_TASK_CORE_ANY,
                         .min_stack_size = 65536u,
                         .stack_region = H2_ESP_TASK_STACK_PSRAM,
                     }};
}

int main(void) {
  reset();
  assert(h2_esp_platform_start_entry_task(NULL, entry, &s, cleanup, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_platform_start_entry_task("", entry, &s, cleanup, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_platform_start_entry_task("entry/test", NULL, &s, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(s.reserves == 0 && s.allocations == 0);

  const h2_pal_result_t errors[] = {H2_PAL_ERR_INVALID_STATE,
                                    H2_PAL_ERR_NOT_FOUND, H2_PAL_ERR_TASK};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    reset();
    s.reserve_result = errors[i];
    assert(start(cleanup) == errors[i]);
    assert(s.reserves == 1 && s.allocations == 0 && s.starts == 0);
    assert(s.entries == 0 && s.cleanups == 0);
  }
  reset();
  s.fail_alloc = 1;
  assert(start(cleanup) == H2_PAL_ERR_NO_MEMORY);
  assert(s.allocations == 1 && s.frees == 0 && s.starts == 0);
  assert(s.entries == 0 && s.cleanups == 0);

  for (int internal = 0; internal < 2; ++internal) {
    reset();
    if (internal) s.policy.stack_region = H2_ESP_TASK_STACK_INTERNAL;
    s.fail_create = 1;
    assert(start(cleanup) == H2_PAL_ERR_TASK);
    assert(s.allocations == 1 && s.frees == 1 && s.starts == 1);
    assert(s.entries == 0 && s.cleanups == 0 && s.deletes == 0);
    for (int with_cleanup = 0; with_cleanup < 2; ++with_cleanup) {
      reset();
      if (internal) {
        s.policy = (h2_esp_task_policy_t){
            .priority = 7u,
            .core = H2_ESP_TASK_CORE_1,
            .min_stack_size = 2048u,
            .stack_region = H2_ESP_TASK_STACK_INTERNAL,
        };
      }
      assert(start(with_cleanup ? cleanup : NULL) == H2_PAL_OK);
      assert(s.allocations == 1 && s.frees == 0 && s.starts == 1);
      assert(s.priority == (internal ? 7u : 4u));
      assert(s.core == (internal ? 1 : tskNO_AFFINITY));
      assert(s.stack_size == (internal ? 4096u : 65536u));
      assert(s.context_caps ==
             (unsigned int)(MALLOC_CAP_8BIT | (internal ? MALLOC_CAP_INTERNAL
                                                        : MALLOC_CAP_SPIRAM)));
      assert(s.stack_caps == (internal ? 0u : s.context_caps));
      s.trampoline(s.context);
      assert(s.frees == 1 && s.entries == 1 && s.deletes == 1);
      assert(s.cleanups == with_cleanup);
    }
  }
  return 0;
}
