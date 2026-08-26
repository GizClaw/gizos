#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>

struct h2_pal_task {
  beken_thread_t thread;
  beken_semaphore_t done;
  h2_pal_task_entry_t entry;
  void *ctx;
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

static const char *bk_task_sdk_name(const char *name,
                                    const h2_bk_task_policy_t *policy) {
  return policy->sdk_name != NULL && policy->sdk_name[0] != '\0'
             ? policy->sdk_name
             : bk_task_portable_name(name);
}

static void bk_task_fail(const char *name, const char *stage,
                         const char *reason) {
  printf("H2_PAL_TASK_POLICY_FAIL name=%s stage=%s reason=%s\n",
         bk_task_portable_name(name), stage, reason);
}

static bool bk_task_policy_shape_valid(const h2_bk_task_policy_t *policy) {
  return policy != NULL && policy->priority <= UINT8_MAX &&
         policy->min_stack_size != 0u &&
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

static void bk_task_trampoline(void *raw) {
  h2_pal_task_t *task = (h2_pal_task_t *)raw;
  task->entry(task->ctx);
  (void)rtos_set_semaphore(&task->done);
  rtos_delete_thread(NULL);
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
  if (!bk_task_policy_shape_valid(&policy) ||
      options->min_stack_size > UINT32_MAX) {
    bk_task_fail(options->name, "validate", "invalid-policy");
    return H2_PAL_ERR_TASK;
  }
  uint32_t stack_size = (uint32_t)options->min_stack_size;
  if (stack_size < 4096u) {
    stack_size = 4096u;
  }
  if (stack_size < policy.min_stack_size) {
    stack_size = policy.min_stack_size;
  }

  h2_pal_task_t *task = (h2_pal_task_t *)os_malloc(sizeof(*task));
  if (task == NULL) {
    bk_task_fail(options->name, "allocate", "task");
    return H2_PAL_ERR_NO_MEMORY;
  }
  os_memset(task, 0, sizeof(*task));
  if (rtos_init_semaphore(&task->done, 1) != kNoErr) {
    os_free(task);
    bk_task_fail(options->name, "allocate", "semaphore");
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->entry = entry;
  task->ctx = ctx;

  const char *sdk_name = bk_task_sdk_name(options->name, &policy);
  int ret =
      policy.stack_region == H2_BK_TASK_STACK_PSRAM
          ? rtos_create_psram_thread(
                &task->thread, (uint8_t)policy.priority, sdk_name,
                (beken_thread_function_t)bk_task_trampoline, stack_size, task)
          : rtos_create_thread(
                &task->thread, (uint8_t)policy.priority, sdk_name,
                (beken_thread_function_t)bk_task_trampoline, stack_size, task);
  if (ret != kNoErr) {
    rtos_deinit_semaphore(&task->done);
    os_free(task);
    bk_task_fail(options->name, "create", "sdk");
    return H2_PAL_ERR_TASK;
  }
  printf(
      "H2_PAL_TASK_READY name=%s sdk_name=%s priority=%lu stack=%s size=%lu\n",
      bk_task_portable_name(options->name), sdk_name,
      (unsigned long)policy.priority,
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
  int ret = rtos_get_semaphore(&task->done, BEKEN_WAIT_FOREVER);
  if (ret != kNoErr) {
    return H2_PAL_ERR_TASK;
  }
  rtos_deinit_semaphore(&task->done);
  os_free(task);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config) {
  if (s_task_configured || s_task_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (config == NULL || config->resolver == NULL) {
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
