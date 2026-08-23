#include "h2_windows_internal.h"

struct h2_pal_task {
    h2_windows_platform_t *platform;
    HANDLE thread;
    h2_pal_task_entry_t entry;
    void *context;
};

static DWORD WINAPI windows_task_entry(void *user) {
    h2_pal_task_t *task = user;
    task->entry(task->context);
    return 0u;
}

static int windows_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *context,
    h2_pal_task_t **out_task) {
    h2_windows_platform_t *platform = user;
    if (entry == NULL || out_task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_task = NULL;
    h2_pal_task_t *task = h2_windows_heap_alloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->platform = platform;
    task->entry = entry;
    task->context = context;
    SIZE_T stack_size = options == NULL ? 0u : (SIZE_T)options->min_stack_size;
    task->thread = CreateThread(NULL, stack_size, windows_task_entry, task, 0u,
                                NULL);
    if (task->thread == NULL) {
        h2_windows_heap_free(task);
        return H2_PAL_ERR_TASK;
    }
    h2_windows_object_acquire(platform);
    *out_task = task;
    return H2_PAL_OK;
}

static int windows_task_join(void *user, h2_pal_task_t *task) {
    h2_windows_platform_t *platform = user;
    if (task == NULL || task->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DWORD result = WaitForSingleObject(task->thread, INFINITE);
    if (result != WAIT_OBJECT_0 || !CloseHandle(task->thread)) {
        return H2_PAL_ERR_TASK;
    }
    h2_windows_heap_free(task);
    h2_windows_object_release(platform);
    return H2_PAL_OK;
}

const h2_pal_task_vtable_t h2_windows_task_vtable = {
    .start = windows_task_start,
    .join = windows_task_join,
};
