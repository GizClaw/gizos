#include "h2_jieli_br23_platform_core.h"
#include "h2_jieli_br23_sdk_port.h"

#include "h2_jieli_br23_atomic.h"
#include <stdio.h>
#include <string.h>

#ifndef H2_JIELI_BR23_TASK_DEFAULT_STACK_BYTES
#define H2_JIELI_BR23_TASK_DEFAULT_STACK_BYTES 4096u
#endif

struct h2_pal_task {
    h2_pal_task_entry_t entry;
    void *ctx;
    h2_jieli_sdk_sem_t *done;
    /* Owned by the joining caller; retain completion across delete retries. */
    int completion_observed;
    /* Unique native identity retained until the joining caller deletes it. */
    char name[H2_JIELI_BR23_TASK_NAME_MAX];
};

static void task_trampoline(void *arg)
{
    h2_pal_task_t *task = (h2_pal_task_t *)arg;
    task->entry(task->ctx);
    /* Publishing completion hands the task, its semaphore and this handle to
     * the joining caller; nothing may touch them after the give returns. */
    (void)h2_jieli_sdk_sem_give(task->done);
    /* SDK tasks must never return. Parking holds no SDK lock, so the joining
     * caller can delete this task and reclaim its stack and TCB. */
    h2_jieli_sdk_task_park();
}

static int task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task)
{
    /* The SDK deletes tasks by name, so a live task needs a distinct one. */
    static volatile uint32_t sequence;
    h2_pal_task_t *task;
    size_t stack_bytes = H2_JIELI_BR23_TASK_DEFAULT_STACK_BYTES;
    uint32_t id;
    (void)user;
    if (entry == NULL || out_task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_task = NULL;
    if (options != NULL) {
        if (options->min_stack_size > stack_bytes) {
            stack_bytes = options->min_stack_size;
        }
    }
    task = (h2_pal_task_t *)h2_jieli_sdk_malloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(task, 0, sizeof(*task));
    task->entry = entry;
    task->ctx = ctx;
    task->completion_observed = 0;
    task->done = h2_jieli_sdk_sem_create(0u);
    if (task->done == NULL) {
        h2_jieli_sdk_free(task);
        return H2_PAL_ERR_NO_MEMORY;
    }
    id = h2_jieli_atomic_load_u32(&sequence);
    while (!h2_jieli_atomic_cas_u32(&sequence, &id, id + 1u)) {
    }
    /* A PAL name is a caller label and several live tasks may share it, while
     * the SDK keeps only H2_JIELI_BR23_TASK_NAME_MAX - 1 characters and treats
     * that string as the task identity. The generated identity fills the
     * buffer exactly, so options->name stays PAL-side metadata. */
    snprintf(task->name, sizeof(task->name), "h2_%08x", (unsigned)id);
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
    if (!task->completion_observed) {
        if (h2_jieli_sdk_sem_take(task->done, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
            return H2_PAL_ERR_TASK;
        }
        task->completion_observed = 1;
    }
    /* The entry has returned and the worker is parked in os_time_dly(), so
     * deleting it here releases its stack and TCB immediately instead of
     * leaving a self-deleted task waiting for idle time that a busy product
     * never grants. A failed delete keeps every resource for a retry. */
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

const h2_pal_task_api_t *h2_jieli_br23_platform_task_api(void)
{
    return &s_task_api;
}
