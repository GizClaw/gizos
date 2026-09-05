#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <string.h>

#ifndef H2_JIELI_WL82_TASK_DEFAULT_STACK_BYTES
#define H2_JIELI_WL82_TASK_DEFAULT_STACK_BYTES 4096u
#endif

struct h2_pal_task {
    h2_pal_task_entry_t entry;
    void *ctx;
    h2_jieli_sdk_sem_t *done;
    /* JieLi task names used by shared transports can exceed 15 characters
     * (for example, "h2loader/blelink"). Preserve the full registered name
     * for both create and delete. */
    char name[32];
};

static void task_trampoline(void *arg)
{
    h2_pal_task_t *task = (h2_pal_task_t *)arg;
    task->entry(task->ctx);
    (void)h2_jieli_sdk_sem_give(task->done);
    /* SDK tasks must never return; park the task until the system resets. */
    h2_jieli_sdk_task_park();
}

static int task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task)
{
    h2_pal_task_t *task;
    size_t stack_bytes = H2_JIELI_WL82_TASK_DEFAULT_STACK_BYTES;
    const char *name = "h2_task";
    (void)user;
    if (entry == NULL || out_task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_task = NULL;
    if (options != NULL) {
        if (options->min_stack_size > stack_bytes) {
            stack_bytes = options->min_stack_size;
        }
        if (options->name != NULL && options->name[0] != '\0') {
            name = options->name;
        }
    }
    task = (h2_pal_task_t *)h2_jieli_sdk_malloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(task, 0, sizeof(*task));
    task->entry = entry;
    task->ctx = ctx;
    task->done = h2_jieli_sdk_sem_create(0u);
    if (task->done == NULL) {
        h2_jieli_sdk_free(task);
        return H2_PAL_ERR_NO_MEMORY;
    }
    strncpy(task->name, name, sizeof(task->name) - 1u);
    if (h2_jieli_sdk_task_create(task_trampoline, task, task->name, stack_bytes) != 0) {
        h2_jieli_sdk_sem_destroy(task->done);
        h2_jieli_sdk_free(task);
        return H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static int task_join(void *user, h2_pal_task_t *task)
{
    (void)user;
    if (task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_jieli_sdk_sem_take(task->done, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_TASK;
    }
    if (h2_jieli_sdk_task_delete(task->name) != 0) {
        return H2_PAL_ERR_TASK;
    }
    h2_jieli_sdk_sem_destroy(task->done);
    h2_jieli_sdk_free(task);
    return H2_PAL_OK;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = task_start,
    .join = task_join,
};

static const h2_pal_task_api_t s_task_api = {
    .user = NULL,
    .vtable = &s_task_vtable,
};

const h2_pal_task_api_t *h2_jieli_wl82_platform_task_api(void)
{
    return &s_task_api;
}
