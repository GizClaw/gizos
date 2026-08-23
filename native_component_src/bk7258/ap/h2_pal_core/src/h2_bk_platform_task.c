#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

struct h2_pal_task {
    beken_thread_t thread;
    beken_semaphore_t done;
    StaticSemaphore_t done_storage;
    const h2_pal_mem_api_t *allocator;
    h2_pal_task_entry_t entry;
    void *ctx;
};

typedef struct h2_bk_task_resolved_policy {
    uint8_t core;
    uint8_t priority;
    uint32_t min_stack_size;
    h2_bk_task_stack_region_t stack_region;
    int fallback;
} h2_bk_task_resolved_policy_t;

static const h2_bk_task_policy_t s_h2_bk_task_policies[] = {
    { "cmd/h2test", 0u, 7u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "cmd/apploader", 0u, 7u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "ble_speed_p", 0u, 6u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "ble_speed_c", 0u, 6u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "h2peer/net", 0u, 6u, 16384u, H2_BK_TASK_STACK_PSRAM },
    { "h2peer/udp", 0u, 6u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "audio-system-music", 1u, 4u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "audio-system-mic", 1u, 4u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "mfg-music", 1u, 4u, 4096u, H2_BK_TASK_STACK_PSRAM },
    { "mfg-mic", 1u, 4u, 4096u, H2_BK_TASK_STACK_PSRAM },
};

static h2_bk_task_policy_resolver_t s_h2_bk_task_policy_resolver;
static void *s_h2_bk_task_policy_resolver_user;
static const h2_pal_mem_api_t *s_h2_bk_task_allocator;
static bool s_h2_bk_task_reject_unknown;

static const char *bk_task_name_for_name(const char *name) {
    if (name != NULL && strcmp(name, "cmd/h2test") == 0) {
        return "h2test";
    }
    if (name != NULL && strcmp(name, "cmd/apploader") == 0) {
        return "h2loader";
    }
    return name != NULL ? name : "h2_task";
}

static bool bk_task_policy_for_name(
    const char *name,
    h2_bk_task_resolved_policy_t *out_resolved) {
    h2_bk_task_resolved_policy_t resolved = {
        .core = 0u,
        .priority = BEKEN_APPLICATION_PRIORITY,
        .min_stack_size = 4096u,
        .stack_region = H2_BK_TASK_STACK_DEFAULT,
        .fallback = 1,
    };

    for (size_t i = 0u;
         i < sizeof(s_h2_bk_task_policies) / sizeof(s_h2_bk_task_policies[0]);
         ++i) {
        const h2_bk_task_policy_t *policy = &s_h2_bk_task_policies[i];
        if (name != NULL && strcmp(name, policy->name) == 0) {
            resolved.core = policy->core;
            resolved.priority = policy->priority;
            resolved.min_stack_size = policy->min_stack_size;
            resolved.stack_region = policy->stack_region;
            resolved.fallback = 0;
            *out_resolved = resolved;
            return true;
        }
    }
    if (s_h2_bk_task_policy_resolver != NULL) {
        h2_bk_task_policy_t policy;
        os_memset(&policy, 0, sizeof(policy));
        if (s_h2_bk_task_policy_resolver(
                s_h2_bk_task_policy_resolver_user, name, &policy)) {
            if (policy.core > 1u || policy.min_stack_size == 0u ||
                policy.stack_region != H2_BK_TASK_STACK_PSRAM) {
                return false;
            }
            resolved.core = policy.core;
            resolved.priority = policy.priority;
            resolved.min_stack_size = policy.min_stack_size;
            resolved.stack_region = policy.stack_region;
            resolved.fallback = 0;
            *out_resolved = resolved;
            return true;
        }
        if (s_h2_bk_task_reject_unknown) {
            return false;
        }
    }
    if (name != NULL &&
        strncmp(name, "bleikcp-speed/", strlen("bleikcp-speed/")) == 0) {
        resolved.priority = BEKEN_DEFAULT_WORKER_PRIORITY;
        resolved.stack_region = H2_BK_TASK_STACK_PSRAM;
        resolved.fallback = 0;
    }
    *out_resolved = resolved;
    return true;
}

static int bk_task_create(
    beken_thread_t *thread,
    const h2_bk_task_resolved_policy_t *policy,
    const char *name,
    beken_thread_function_t entry,
    uint32_t stack_size,
    void *ctx) {
#if defined(CONFIG_SOC_SMP) && CONFIG_SOC_SMP && \
    defined(CONFIG_CPU_CNT) && CONFIG_CPU_CNT > 1
    if (policy->core == 1u) {
        if (policy->stack_region == H2_BK_TASK_STACK_PSRAM) {
            return rtos_core1_create_psram_thread(
                thread, policy->priority, name, entry, stack_size, ctx);
        }
        return rtos_core1_create_thread(
            thread, policy->priority, name, entry, stack_size, ctx);
    }
    if (policy->stack_region == H2_BK_TASK_STACK_PSRAM) {
        return rtos_core0_create_psram_thread(
            thread, policy->priority, name, entry, stack_size, ctx);
    }
    return rtos_core0_create_thread(
        thread, policy->priority, name, entry, stack_size, ctx);
#else
    if (policy->stack_region == H2_BK_TASK_STACK_PSRAM) {
        return rtos_create_psram_thread(
            thread, policy->priority, name, entry, stack_size, ctx);
    }
    return rtos_create_thread(
        thread, policy->priority, name, entry, stack_size, ctx);
#endif
}

static void bk_task_trampoline(void *raw) {
    h2_pal_task_t *task = (h2_pal_task_t *)raw;
    task->entry(task->ctx);
    (void)xSemaphoreGive((SemaphoreHandle_t)task->done);
    rtos_delete_thread(NULL);
}

static void *bk_task_alloc(size_t size) {
    return s_h2_bk_task_allocator != NULL
        ? h2_pal_mem_alloc(s_h2_bk_task_allocator, size)
        : os_malloc(size);
}

static void bk_task_free(h2_pal_task_t *task) {
    if (task == NULL) {
        return;
    }
    if (task->allocator != NULL) {
        h2_pal_mem_free(task->allocator, task);
    } else {
        os_free(task);
    }
}

static int bk_task_start(void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;

    if (options == NULL || entry == NULL || out_task == NULL) {
        return -1;
    }

    h2_bk_task_resolved_policy_t policy;
    if (!bk_task_policy_for_name(options->name, &policy)) {
        printf("H2_PAL_TASK_POLICY_MISS name=%s\n",
            bk_task_name_for_name(options->name));
        return H2_PAL_ERR_TASK;
    }

    h2_pal_task_t *task = (h2_pal_task_t *)bk_task_alloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(task, 0, sizeof(*task));
    task->allocator = s_h2_bk_task_allocator;
    task->done = (beken_semaphore_t)xSemaphoreCreateBinaryStatic(
        &task->done_storage);
    if (task->done == NULL) {
        bk_task_free(task);
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->entry = entry;
    task->ctx = ctx;

    uint32_t stack_size =
        options->min_stack_size > 0u ? (uint32_t)options->min_stack_size : 4096u;
    if (stack_size < policy.min_stack_size) {
        stack_size = policy.min_stack_size;
    }

    const char *task_name = bk_task_name_for_name(options->name);
    int ret = bk_task_create(
        &task->thread,
        &policy,
        task_name,
        (beken_thread_function_t)bk_task_trampoline,
        stack_size,
        task);
    if (ret != kNoErr) {
        printf("H2_PAL_TASK_ERROR op=create name=%s ret=%d core=%u "
               "priority=%u stack=%s size=%lu\n",
            task_name,
            ret,
            (unsigned int)policy.core,
            (unsigned int)policy.priority,
            policy.stack_region == H2_BK_TASK_STACK_PSRAM
                ? "psram"
                : "default",
            (unsigned long)stack_size);
        bk_task_free(task);
        return H2_PAL_ERR_TASK;
    }
    if (policy.fallback) {
        printf("H2_PAL_TASK_FALLBACK name=%s core=%u priority=%u "
               "stack=%s size=%lu\n",
            task_name,
            (unsigned int)policy.core,
            (unsigned int)policy.priority,
            policy.stack_region == H2_BK_TASK_STACK_PSRAM
                ? "psram"
                : "default",
            (unsigned long)stack_size);
    }
    printf("H2_PAL_TASK_READY name=%s core=%u priority=%u stack=%s size=%lu\n",
        task_name,
        (unsigned int)policy.core,
        (unsigned int)policy.priority,
        policy.stack_region == H2_BK_TASK_STACK_PSRAM ? "psram" : "default",
        (unsigned long)stack_size);

    *out_task = task;
    return 0;
}

static int bk_task_join(void *user, h2_pal_task_t *task) {
    (void)user;

    if (task == NULL) {
        return -1;
    }
    BaseType_t ret = xSemaphoreTake(
        (SemaphoreHandle_t)task->done, portMAX_DELAY);
    if (ret != pdPASS) {
        printf("H2_PAL_TASK_ERROR op=join ret=%d\n", ret);
        return H2_PAL_ERR_TASK;
    }
    bk_task_free(task);
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk_platform_task_configure(
    h2_bk_task_policy_resolver_t resolver,
    void *resolver_user,
    const h2_pal_mem_api_t *allocator,
    bool reject_unknown) {
    if (resolver == NULL || allocator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_bk_task_policy_resolver = resolver;
    s_h2_bk_task_policy_resolver_user = resolver_user;
    s_h2_bk_task_allocator = allocator;
    s_h2_bk_task_reject_unknown = reject_unknown;
    return H2_PAL_OK;
}

const h2_pal_task_api_t *h2_bk_platform_task_api(void) {
    static const h2_pal_task_vtable_t vtable = {
        .start = bk_task_start,
        .join = bk_task_join,
    };
    static const h2_pal_task_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
