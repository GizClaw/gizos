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
    /* Published by the worker before it runs the entry and read by joining
     * tasks. The worker always observes its own write; a task on the other
     * core may read a stale NULL, which only skips a self-join check it could
     * never satisfy anyway. */
    const void *volatile self;
    /* Owned by the joining caller; retain completion across delete retries. */
    int completion_observed;
    /* Unique native identity retained until the joining caller deletes it. */
    char name[32];
};

static void task_trampoline(void *arg)
{
    h2_pal_task_t *task = (h2_pal_task_t *)arg;
    /* Identify this worker before the entry can hand the handle around, so a
     * self-join is rejected instead of waiting for a completion that only this
     * task could publish. */
    task->self = h2_jieli_sdk_task_current();
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
    const char *name = NULL;
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
    /* Truncation changes the SDK policy lookup key and can collide with a
     * different task. Reject it before allocating any SDK resources. */
    if (name != NULL && (strlen(name) >= sizeof(task->name) ||
                         strncmp(name, "$h2anon/", 8u) == 0)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    task = (h2_pal_task_t *)h2_jieli_sdk_malloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(task, 0, sizeof(*task));
    task->completion_observed = 0;
    task->entry = entry;
    task->ctx = ctx;
    task->done = h2_jieli_sdk_sem_create(0u);
    if (task->done == NULL) {
        h2_jieli_sdk_free(task);
        return H2_PAL_ERR_NO_MEMORY;
    }
    /* PAL names select scheduling policy; they are labels, not unique task
     * identities. Several live workers may use the same policy. The SDK
     * deletes by name, so retain a distinct native name for every live object. */
    const char *label = name != NULL ? name : "$h2anon";
    /* The SDK strips affinity metadata before storing its native task name.
     * Retain that same canonical identity for name-based deletion. */
    if (strncmp(label, "#C", 2u) == 0 &&
        (label[2] == '0' || label[2] == '1')) label += 3u;
    static const char hex[] = "0123456789abcdef";
    const uintptr_t identity = (uintptr_t)task;
    const size_t digits = 2u * sizeof(identity);
    const size_t prefix_limit = sizeof(task->name) - digits - 2u;
    size_t prefix = strlen(label);
    if (prefix > prefix_limit) prefix = prefix_limit;
    memcpy(task->name, label, prefix);
    task->name[prefix++] = '/';
    for (size_t i = 0; i < digits; ++i) {
        task->name[prefix + i] = hex[(identity >>
            (4u * (digits - i - 1u))) & 15u];
    }
    task->name[prefix + digits] = '\0';
    if (h2_jieli_sdk_task_create(task_trampoline, task, name, task->name, stack_bytes) != 0) {
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
    /* Only the worker publishes completion, so a worker joining itself would
     * block forever. A handle is unique while its task lives; an unpublished
     * NULL never matches a running task. */
    if (task->self != NULL && task->self == h2_jieli_sdk_task_current()) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!task->completion_observed) {
        if (h2_jieli_sdk_sem_take(task->done, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
            return H2_PAL_ERR_TASK;
        }
        task->completion_observed = 1;
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
