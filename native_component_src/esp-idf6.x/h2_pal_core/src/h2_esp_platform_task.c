#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#define H2_ESP_HAS_PSRAM 1
#else
#define H2_ESP_HAS_PSRAM 0
#endif

#if H2_ESP_HAS_PSRAM &&                                                        \
    (!defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) || !CONFIG_SPIRAM_XIP_FROM_PSRAM)
#error "ESP targets with PSRAM must enable CONFIG_SPIRAM_XIP_FROM_PSRAM"
#endif

struct h2_pal_task {
  TaskHandle_t task;
  SemaphoreHandle_t done;
  h2_pal_task_entry_t entry;
  void *ctx;
  int stack_with_caps;
  const h2_pal_mem_api_t *stack_allocator;
  StackType_t *stack;
  StaticTask_t *task_storage;
};

static h2_esp_task_policy_config_t s_task_config;
static bool s_task_configured;
static bool s_task_started;

#if defined(H2_TASK_POLICY_TEST)
void h2_esp_platform_task_test_reset(void) {
  s_task_config = (h2_esp_task_policy_config_t){0};
  s_task_configured = false;
  s_task_started = false;
}
#endif

static const char *esp_task_name(const char *name) {
  return name != NULL && name[0] != '\0' ? name : "h2_task";
}

static void esp_task_fail(const char *name, const char *stage,
                          const char *reason) {
  printf("H2_PAL_TASK_POLICY_FAIL name=%s stage=%s reason=%s\n",
         esp_task_name(name), stage, reason);
}

static bool esp_policy_shape_valid(const h2_esp_task_policy_t *policy) {
  return policy != NULL && policy->min_stack_size != 0u &&
         (policy->core == H2_ESP_TASK_CORE_ANY || policy->core >= 0) &&
         (policy->stack_region == H2_ESP_TASK_STACK_INTERNAL ||
          policy->stack_region == H2_ESP_TASK_STACK_PSRAM);
}

static BaseType_t esp_policy_core(const h2_esp_task_policy_t *policy) {
  return policy->core == H2_ESP_TASK_CORE_ANY ? tskNO_AFFINITY
                                              : (BaseType_t)policy->core;
}

static h2_pal_result_t esp_policy_resolve(const char *name,
                                          h2_esp_task_policy_t *out_policy) {
  h2_pal_result_t rc =
      s_task_config.resolver(s_task_config.resolver_user, name, out_policy);
  if (rc == H2_PAL_OK) {
    return H2_PAL_OK;
  }
  if (rc != H2_PAL_ERR_NOT_FOUND) {
    esp_task_fail(name, "resolve", "resolver-error");
    return H2_PAL_ERR_TASK;
  }
  esp_task_fail(name, "resolve", "not-found");
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t esp_policy_validate(const char *name,
                                           const h2_esp_task_policy_t *policy) {
  if (!esp_policy_shape_valid(policy) ||
      policy->priority >= configMAX_PRIORITIES) {
    esp_task_fail(name, "validate", "invalid-policy");
    return H2_PAL_ERR_TASK;
  }
  if (policy->core != H2_ESP_TASK_CORE_ANY &&
      (uint32_t)policy->core >= (uint32_t)CONFIG_FREERTOS_NUMBER_OF_CORES) {
    esp_task_fail(name, "validate", "unavailable-core");
    return H2_PAL_ERR_TASK;
  }
  if (policy->stack_region == H2_ESP_TASK_STACK_PSRAM &&
      (!H2_ESP_HAS_PSRAM || !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)) {
    esp_task_fail(name, "validate", "psram-unavailable");
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}

/* IDF WithCaps self-deletion allocates a temporary cleanup task, which then
 * self-deletes and needs idle time for reclamation. Let join reclaim PSRAM
 * workers directly, without allocating another task under memory pressure.
 * Internal stacks keep self-deletion so delayed joins do not retain scarce
 * Internal memory needed during startup. */
static void esp_task_trampoline(void *raw) {
  h2_pal_task_t *task = (h2_pal_task_t *)raw;
  const int stack_with_caps = task->stack_with_caps || task->stack_allocator != NULL;
  task->entry(task->ctx);
  /* Join may free the PAL handle as soon as this signal is consumed. Do not
   * access task or the entry context after giving the semaphore. */
  xSemaphoreGive(task->done);
  if (!stack_with_caps) {
    /* An Internal stack is small and scarce: let the idle task reclaim it as
     * soon as it runs rather than hold it until join. */
    vTaskDelete(NULL);
  }
  for (;;) {
    vTaskSuspend(NULL);
  }
}

static int esp_task_start(void *user, const h2_pal_task_options_t *options,
                          h2_pal_task_entry_t entry, void *ctx,
                          h2_pal_task_t **out_task) {
  (void)user;
  if (out_task != NULL) {
    *out_task = NULL;
  }
  if (options == NULL || entry == NULL || out_task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_task_started = true;
  if (!s_task_configured) {
    esp_task_fail(options->name, "configure", "not-configured");
    return H2_PAL_ERR_INVALID_STATE;
  }

  h2_esp_task_policy_t policy = {0};
  h2_pal_result_t rc = esp_policy_resolve(options->name, &policy);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = esp_policy_validate(options->name, &policy);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (options->min_stack_size > UINT32_MAX) {
    esp_task_fail(options->name, "validate", "stack-overflow");
    return H2_PAL_ERR_TASK;
  }
  uint32_t stack_size = (uint32_t)options->min_stack_size;
  if (stack_size < 4096u) {
    stack_size = 4096u;
  }
  if (stack_size < policy.min_stack_size) {
    stack_size = policy.min_stack_size;
  }

  h2_pal_task_t *task = (h2_pal_task_t *)calloc(1u, sizeof(*task));
  if (task == NULL) {
    esp_task_fail(options->name, "allocate", "task");
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->done = xSemaphoreCreateBinary();
  if (task->done == NULL) {
    free(task);
    esp_task_fail(options->name, "allocate", "semaphore");
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->entry = entry;
  task->ctx = ctx;

  const BaseType_t core = esp_policy_core(&policy);
  BaseType_t ok;
  if (policy.stack_region == H2_ESP_TASK_STACK_INTERNAL) {
    ok = xTaskCreatePinnedToCore(
        esp_task_trampoline, esp_task_name(options->name), stack_size, task,
        (UBaseType_t)policy.priority, &task->task, core);
  } else if (s_task_config.psram_stack_allocator != NULL) {
    task->stack_allocator = s_task_config.psram_stack_allocator;
    task->stack = h2_pal_mem_alloc(task->stack_allocator, stack_size);
    task->task_storage = heap_caps_malloc(
        sizeof(*task->task_storage), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task->stack == NULL || task->task_storage == NULL) {
      h2_pal_mem_free(task->stack_allocator, task->stack);
      heap_caps_free(task->task_storage);
      vSemaphoreDelete(task->done);
      free(task);
      esp_task_fail(options->name, "allocate", "stack-or-tcb");
      return H2_PAL_ERR_NO_MEMORY;
    }
    task->task = xTaskCreateStaticPinnedToCore(
        esp_task_trampoline, esp_task_name(options->name), stack_size, task,
        (UBaseType_t)policy.priority, task->stack, task->task_storage, core);
    ok = task->task != NULL ? pdPASS : 0;
  } else {
    task->stack_with_caps = 1;
    ok = xTaskCreatePinnedToCoreWithCaps(
        esp_task_trampoline, esp_task_name(options->name), stack_size, task,
        (UBaseType_t)policy.priority, &task->task, core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (ok != pdPASS) {
    h2_pal_mem_free(task->stack_allocator, task->stack);
    heap_caps_free(task->task_storage);
    vSemaphoreDelete(task->done);
    free(task);
    esp_task_fail(options->name, "create", "sdk");
    return H2_PAL_ERR_TASK;
  }
  printf("H2_PAL_TASK_READY name=%s priority=%lu core=%ld stack=%s size=%lu\n",
         esp_task_name(options->name), (unsigned long)policy.priority,
         (long)policy.core,
         policy.stack_region == H2_ESP_TASK_STACK_INTERNAL ? "internal"
                                                           : "psram",
         (unsigned long)stack_size);
  *out_task = task;
  return H2_PAL_OK;
}

static int esp_task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (task == NULL || task->done == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (xSemaphoreTake(task->done, portMAX_DELAY) != pdTRUE) {
    return H2_PAL_ERR_TASK;
  }
  /* The entry has returned. IDF's WithCaps deletion suspends the worker and
   * waits for it to stop running before freeing its stack and TCB, including
   * when join wins the race between xSemaphoreGive and vTaskSuspend on another
   * core. Keep the semaphore and PAL handle alive until deletion returns. */
  if (task->stack_allocator != NULL) {
    /* As in IDF's WithCaps deletion, wait until vTaskDelete can reclaim the
     * kernel state synchronously, even if join wins on the other core. */
    vTaskSuspend(task->task);
    for (;;) {
      bool running = false;
      for (BaseType_t core = 0; core < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core) {
        if (xTaskGetCurrentTaskHandleForCore(core) == task->task) {
          running = true;
          break;
        }
      }
      if (!running) break;
      taskYIELD();
    }
    vTaskDelete(task->task);
    h2_pal_mem_free(task->stack_allocator, task->stack);
    heap_caps_free(task->task_storage);
  } else if (task->stack_with_caps) {
    vTaskDeleteWithCaps(task->task);
  }
  vSemaphoreDelete(task->done);
  free(task);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config) {
  if (s_task_configured || s_task_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (config == NULL || config->resolver == NULL) {
    esp_task_fail(NULL, "configure", "invalid-config");
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator = config->psram_stack_allocator;
  if (allocator != NULL &&
      (allocator->vtable == NULL || allocator->vtable->alloc == NULL ||
       allocator->vtable->free == NULL)) {
    esp_task_fail(NULL, "configure", "invalid-stack-allocator");
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_task_config = *config;
  s_task_configured = true;
  return H2_PAL_OK;
}

const h2_pal_task_api_t *h2_esp_platform_task_api(void) {
  static const h2_pal_task_vtable_t vtable = {
      .start = esp_task_start,
      .join = esp_task_join,
  };
  static const h2_pal_task_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}
