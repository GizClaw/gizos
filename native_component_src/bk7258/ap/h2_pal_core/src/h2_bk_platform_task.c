#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

struct h2_pal_task {
  beken_thread_t thread;
  beken_semaphore_t done;
  StaticSemaphore_t done_storage;
  const h2_pal_mem_api_t *allocator;
  h2_pal_task_entry_t entry;
  void *ctx;
  bool join_reclaims;
};

static h2_bk_task_policy_config_t s_task_config;
static bool s_task_configured;
static bool s_task_started;

#if defined(H2_TASK_POLICY_TEST)
void h2_bk_platform_task_test_reset(void) {
  s_task_config = (h2_bk_task_policy_config_t){0};
  s_task_configured = false;
  s_task_started = false;
}
#endif

static const char *bk_task_portable_name(const char *name) {
  return name != NULL && name[0] != '\0' ? name : "h2_task";
}

static const char *bk_task_sdk_name(const char *portable_name,
                                    const h2_bk_task_policy_t *policy) {
  return policy->sdk_name != NULL && policy->sdk_name[0] != '\0'
             ? policy->sdk_name
             : bk_task_portable_name(portable_name);
}

static void bk_task_fail(const char *name, const char *stage,
                         const char *reason) {
  printf("H2_PAL_TASK_POLICY_FAIL name=%s stage=%s reason=%s\n",
         bk_task_portable_name(name), stage, reason);
}

static bool bk_task_policy_shape_valid(const h2_bk_task_policy_t *policy) {
  return policy != NULL && policy->core <= 1u &&
         policy->priority <= UINT8_MAX && policy->min_stack_size != 0u &&
         (policy->stack_region == H2_BK_TASK_STACK_DEFAULT ||
          policy->stack_region == H2_BK_TASK_STACK_PSRAM);
}

static h2_pal_result_t bk_task_policy_resolve(const char *name,
                                              h2_bk_task_policy_t *out_policy) {
  h2_pal_result_t rc =
      s_task_config.resolver(s_task_config.resolver_user, name, out_policy);
  if (rc == H2_PAL_OK) {
    return H2_PAL_OK;
  }
  if (rc != H2_PAL_ERR_NOT_FOUND) {
    bk_task_fail(name, "resolve", "resolver-error");
    return H2_PAL_ERR_TASK;
  }
  bk_task_fail(name, "resolve", "not-found");
  return H2_PAL_ERR_NOT_FOUND;
}

static int bk_task_create(beken_thread_t *thread,
                          const h2_bk_task_policy_t *policy, const char *name,
                          beken_thread_function_t entry, uint32_t stack_size,
                          void *ctx) {
#if defined(CONFIG_SOC_SMP) && CONFIG_SOC_SMP && defined(CONFIG_CPU_CNT) &&    \
    CONFIG_CPU_CNT > 1
  if (policy->core == 1u) {
    return policy->stack_region == H2_BK_TASK_STACK_PSRAM
               ? rtos_core1_create_psram_thread(thread,
                                                (uint8_t)policy->priority, name,
                                                entry, stack_size, ctx)
               : rtos_core1_create_thread(thread, (uint8_t)policy->priority,
                                          name, entry, stack_size, ctx);
  }
  return policy->stack_region == H2_BK_TASK_STACK_PSRAM
             ? rtos_core0_create_psram_thread(thread, (uint8_t)policy->priority,
                                              name, entry, stack_size, ctx)
             : rtos_core0_create_thread(thread, (uint8_t)policy->priority, name,
                                        entry, stack_size, ctx);
#else
  if (policy->core != 0u) {
    return kGeneralErr;
  }
  return policy->stack_region == H2_BK_TASK_STACK_PSRAM
             ? rtos_create_psram_thread(thread, (uint8_t)policy->priority, name,
                                        entry, stack_size, ctx)
             : rtos_create_thread(thread, (uint8_t)policy->priority, name,
                                  entry, stack_size, ctx);
#endif
}

/* Self-deletion only queues the stack and TCB on the kernel's termination list;
 * the idle task of the owning core has to run before prvDeleteTCB() releases
 * them. A PSRAM stack therefore stays allocated for as long as the product
 * starves idle. Let join reclaim those workers directly instead.
 *
 * Default-region stacks come from the internal heap that CONFIG_CUSTOMIZE_HEAP_SIZE
 * caps at 160 KiB, and every BK image assigns that region only to long-lived
 * singletons. Holding one until a late join would take scarce startup memory
 * for no reclamation benefit, so those keep self-deleting. */
static void bk_task_trampoline(void *raw) {
  h2_pal_task_t *task = (h2_pal_task_t *)raw;
  const bool join_reclaims = task->join_reclaims;
  task->entry(task->ctx);
  /* Join may free the PAL handle as soon as this signal is consumed. Do not
   * access task or the entry context after giving the semaphore. */
  (void)xSemaphoreGive((SemaphoreHandle_t)task->done);
  if (!join_reclaims) {
    rtos_delete_thread(NULL);
  }
  for (;;) {
    rtos_suspend_thread(NULL);
  }
}

/* The entry has returned, so the worker only has to leave the CPU. vTaskSuspend()
 * merely sends a yield request to the other core, and vTaskDelete() re-queues a
 * still-running task for idle cleanup, so wait for the scheduler to switch the
 * worker out before deleting it. Only then does rtos_delete_thread() free the
 * stack and TCB on this thread. */
static void bk_task_reclaim(h2_pal_task_t *task) {
  rtos_suspend_thread(&task->thread);
  while (eTaskGetState((TaskHandle_t)task->thread) == eRunning) {
  }
  rtos_delete_thread(&task->thread);
}

static void bk_task_free(h2_pal_task_t *task) {
  if (task != NULL) {
    h2_pal_mem_free(task->allocator, task);
  }
}

static int bk_task_start(void *user, const h2_pal_task_options_t *options,
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
    bk_task_fail(options->name, "configure", "not-configured");
    return H2_PAL_ERR_INVALID_STATE;
  }

  h2_bk_task_policy_t policy = {0};
  h2_pal_result_t rc = bk_task_policy_resolve(options->name, &policy);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (!bk_task_policy_shape_valid(&policy)) {
    bk_task_fail(options->name, "validate", "invalid-policy");
    return H2_PAL_ERR_TASK;
  }
#if !(defined(CONFIG_SOC_SMP) && CONFIG_SOC_SMP && defined(CONFIG_CPU_CNT) &&  \
      CONFIG_CPU_CNT > 1)
  if (policy.core != 0u) {
    bk_task_fail(options->name, "validate", "unavailable-core");
    return H2_PAL_ERR_TASK;
  }
#endif
  if (options->min_stack_size > UINT32_MAX) {
    bk_task_fail(options->name, "validate", "stack-overflow");
    return H2_PAL_ERR_TASK;
  }
  uint32_t stack_size = (uint32_t)options->min_stack_size;
  if (stack_size < 4096u) {
    stack_size = 4096u;
  }
  if (stack_size < policy.min_stack_size) {
    stack_size = policy.min_stack_size;
  }

  h2_pal_task_t *task = (h2_pal_task_t *)h2_pal_mem_alloc(
      s_task_config.task_allocator, sizeof(*task));
  if (task == NULL) {
    bk_task_fail(options->name, "allocate", "task");
    return H2_PAL_ERR_NO_MEMORY;
  }
  os_memset(task, 0, sizeof(*task));
  task->allocator = s_task_config.task_allocator;
  task->done =
      (beken_semaphore_t)xSemaphoreCreateBinaryStatic(&task->done_storage);
  if (task->done == NULL) {
    bk_task_free(task);
    bk_task_fail(options->name, "allocate", "semaphore");
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->entry = entry;
  task->ctx = ctx;
  task->join_reclaims = policy.stack_region == H2_BK_TASK_STACK_PSRAM;

  const char *sdk_name = bk_task_sdk_name(options->name, &policy);
  int ret = bk_task_create(&task->thread, &policy, sdk_name,
                           (beken_thread_function_t)bk_task_trampoline,
                           stack_size, task);
  if (ret != kNoErr) {
    bk_task_free(task);
    bk_task_fail(options->name, "create", "sdk");
    return H2_PAL_ERR_TASK;
  }
  printf("H2_PAL_TASK_READY name=%s sdk_name=%s core=%lu priority=%lu stack=%s "
         "size=%lu\n",
         bk_task_portable_name(options->name), sdk_name,
         (unsigned long)policy.core, (unsigned long)policy.priority,
         policy.stack_region == H2_BK_TASK_STACK_PSRAM ? "psram" : "default",
         (unsigned long)stack_size);
  *out_task = task;
  return H2_PAL_OK;
}

static int bk_task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)task->done, portMAX_DELAY);
  if (ret != pdPASS) {
    return H2_PAL_ERR_TASK;
  }
  if (task->join_reclaims) {
    bk_task_reclaim(task);
  }
  bk_task_free(task);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config) {
  if (s_task_configured || s_task_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (config == NULL || config->resolver == NULL ||
      config->task_allocator == NULL) {
    bk_task_fail(NULL, "configure", "invalid-config");
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_task_config = *config;
  s_task_configured = true;
  return H2_PAL_OK;
}

const h2_pal_task_api_t *h2_bk_platform_task_api(void) {
  static const h2_pal_task_vtable_t vtable = {
      .start = bk_task_start,
      .join = bk_task_join,
  };
  static const h2_pal_task_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}
