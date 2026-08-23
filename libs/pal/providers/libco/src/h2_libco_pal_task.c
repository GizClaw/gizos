#include "h2_libco_internal.h"

#include <limits.h>
#include <string.h>

struct h2_libco_pal_task {
    h2_libco_t *core;
    h2_libco_task_t *native_task;
    h2_pal_task_entry_t entry;
    void *entry_user;
};

static int h2_libco_pal_task_entry(void *user) {
    h2_libco_pal_task_t *task = user;
    task->entry(task->entry_user);
    return H2_PAL_OK;
}

static int h2_libco_pal_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *entry_user,
    h2_pal_task_t **out_task) {
    h2_libco_t *core = user;
    h2_libco_pal_task_t *task;
    h2_libco_task_options_t native_options = {0};
    size_t requested = options == NULL ? 0u : options->min_stack_size;
    h2_libco_result_t result;

    if (out_task == NULL || *out_task != NULL || entry == NULL ||
        core == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_task = NULL;
    if (requested != 0u) {
        if (requested > SIZE_MAX - (H2_LIBCO_STACK_ALIGNMENT - 1u)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        requested = (requested + H2_LIBCO_STACK_ALIGNMENT - 1u) &
                    ~(H2_LIBCO_STACK_ALIGNMENT - 1u);
        if (requested < H2_LIBCO_MIN_STACK_SIZE) {
            requested = H2_LIBCO_MIN_STACK_SIZE;
        }
        if (requested > UINT_MAX) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        native_options.stack_size = requested;
    }

    task = core->config.alloc(core->config.user, sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(task, 0, sizeof(*task));
    task->core = core;
    task->entry = entry;
    task->entry_user = entry_user;
    result = h2_libco_task_start(core, &native_options,
                                 h2_libco_pal_task_entry, task,
                                 &task->native_task);
    if (result != H2_LIBCO_OK) {
        core->config.free(core->config.user, task);
        return h2_libco_internal_to_pal(result);
    }
    ++core->live_pal_objects;
    *out_task = (h2_pal_task_t *)task;
    return H2_PAL_OK;
}

static int h2_libco_pal_task_join(void *user, h2_pal_task_t *opaque_task) {
    h2_libco_t *core = user;
    h2_libco_pal_task_t *task = (h2_libco_pal_task_t *)opaque_task;
    h2_libco_result_t result;
    int entry_result = H2_PAL_OK;

    if (core == NULL || task == NULL || task->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    result = h2_libco_task_join(core, task->native_task, &entry_result);
    if (result != H2_LIBCO_OK) {
        return h2_libco_internal_to_pal(result);
    }
    if (entry_result == H2_LIBCO_ERR_CANCELLED) {
        entry_result = H2_PAL_EXIT;
    }
    --core->live_pal_objects;
    core->config.free(core->config.user, task);
    return entry_result;
}

h2_pal_result_t h2_libco_pal_task_cancel(h2_libco_t *core,
                                         h2_pal_task_t *opaque_task) {
    h2_libco_pal_task_t *task = (h2_libco_pal_task_t *)opaque_task;
    if (core == NULL || task == NULL || task->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_libco_internal_to_pal(
        h2_libco_task_cancel(core, task->native_task));
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = h2_libco_pal_task_start,
    .join = h2_libco_pal_task_join,
};

void h2_libco_internal_init_pal_apis(h2_libco_t *core) {
    core->task_api = (h2_pal_task_api_t){
        .user = core,
        .vtable = &s_task_vtable,
    };
    core->time_api = (h2_pal_time_api_t){
        .user = core,
        .vtable = h2_libco_internal_time_vtable(),
    };
    core->queue_api = (h2_pal_queue_api_t){
        .user = core,
        .vtable = h2_libco_internal_queue_vtable(),
    };
    core->sync_api = (h2_pal_sync_api_t){
        .user = core,
        .vtable = h2_libco_internal_sync_vtable(),
    };
}

const h2_pal_task_api_t *h2_libco_task_api(h2_libco_t *core) {
    return core == NULL ? NULL : &core->task_api;
}

const h2_pal_time_api_t *h2_libco_time_api(h2_libco_t *core) {
    return core == NULL ? NULL : &core->time_api;
}

const h2_pal_queue_api_t *h2_libco_queue_api(h2_libco_t *core) {
    return core == NULL ? NULL : &core->queue_api;
}

const h2_pal_sync_api_t *h2_libco_sync_api(h2_libco_t *core) {
    return core == NULL ? NULL : &core->sync_api;
}
