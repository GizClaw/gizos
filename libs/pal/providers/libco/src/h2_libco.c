#include "h2_libco_internal.h"

#include <limits.h>
#include <string.h>

#define H2_LIBCO_GUARD_LOW UINT64_C(0x6832636f6c6f7721)
#define H2_LIBCO_GUARD_HIGH UINT64_C(0x6832636f68696768)

static h2_libco_t *s_active_core;
static h2_libco_task_t *s_active_task;

#if defined(__EMSCRIPTEN__)
extern size_t h2_libco_emscripten_context_overhead(void);
extern int h2_libco_emscripten_context_valid(const void *memory, size_t size);
#endif

static bool h2_libco_is_power_of_two(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

bool h2_libco_internal_root_context(const h2_libco_t *core) {
    return core != NULL && !core->callback_active && s_active_core == NULL &&
           co_active() == core->root;
}

bool h2_libco_internal_task_context(const h2_libco_t *core) {
    return core != NULL && s_active_core == core && s_active_task != NULL &&
           core->running == s_active_task && co_active() == s_active_task->context;
}

static bool h2_libco_valid_context(const h2_libco_t *core) {
    return h2_libco_internal_root_context(core) ||
           h2_libco_internal_task_context(core);
}

h2_libco_task_t *h2_libco_internal_current_task(h2_libco_t *core) {
    return h2_libco_internal_task_context(core) ? core->running : NULL;
}

static bool h2_libco_task_owned(const h2_libco_t *core,
                                const h2_libco_task_t *task) {
    return core != NULL && task != NULL && task->owner == core;
}

static bool h2_libco_wait_order_before(uint64_t lhs, uint64_t rhs) {
    /* Modular serial comparison remains ordered across one counter wrap. */
    return lhs != rhs && lhs - rhs > UINT64_MAX / 2u;
}

static void h2_libco_ready_push(h2_libco_t *core, h2_libco_task_t *task) {
    if (task->queued) {
        return;
    }
    task->ready_next = NULL;
    task->queued = true;
    if (core->ready_tail == NULL) {
        core->ready_head = task;
    } else {
        core->ready_tail->ready_next = task;
    }
    core->ready_tail = task;
}

static h2_libco_task_t *h2_libco_ready_pop(h2_libco_t *core) {
    h2_libco_task_t *task = core->ready_head;
    if (task == NULL) {
        return NULL;
    }
    core->ready_head = task->ready_next;
    if (core->ready_head == NULL) {
        core->ready_tail = NULL;
    }
    task->ready_next = NULL;
    task->queued = false;
    return task;
}

static bool h2_libco_guard_valid(const h2_libco_task_t *task) {
    uint64_t low;
    uint64_t high;
    if (task->stack == NULL) {
        return true;
    }
    memcpy(&low, task->stack - sizeof(low), sizeof(low));
    memcpy(&high, task->stack + task->context_size, sizeof(high));
    if (low != H2_LIBCO_GUARD_LOW || high != H2_LIBCO_GUARD_HIGH) {
        return false;
    }
#if defined(__EMSCRIPTEN__)
    return h2_libco_emscripten_context_valid(task->stack,
                                             task->context_size) != 0;
#else
    return true;
#endif
}

static h2_libco_result_t h2_libco_check_guards(h2_libco_t *core) {
    for (h2_libco_task_t *task = core->tasks; task != NULL;
         task = task->all_next) {
        if (!h2_libco_guard_valid(task)) {
            core->faulted = true;
            return H2_LIBCO_ERR_STACK_CORRUPT;
        }
    }
    return core->faulted ? H2_LIBCO_ERR_STACK_CORRUPT : H2_LIBCO_OK;
}

static void h2_libco_detach_join(h2_libco_task_t *task) {
    if (task->join_target != NULL && task->join_target->joiner == task) {
        task->join_target->joiner = NULL;
    }
    task->join_target = NULL;
}

static void h2_libco_complete(h2_libco_task_t *task, int entry_result) {
    h2_libco_t *core = task->owner;
    task->entry_result = entry_result;
    task->state = H2_LIBCO_TASK_COMPLETE;
    task->wait_kind = H2_LIBCO_WAIT_NONE;
    task->deadline_set = false;
    if (task->joiner != NULL && task->joiner->state == H2_LIBCO_TASK_WAITING &&
        task->joiner->wait_kind == H2_LIBCO_WAIT_JOIN) {
        h2_libco_task_t *joiner = task->joiner;
        joiner->state = H2_LIBCO_TASK_READY;
        joiner->wait_kind = H2_LIBCO_WAIT_NONE;
        joiner->resume_result = H2_LIBCO_OK;
        h2_libco_ready_push(core, joiner);
    }
}

static void h2_libco_task_trampoline(void) {
    h2_libco_task_t *task = s_active_task;
    h2_libco_t *core = s_active_core;
    int result = task->entry(task->user);
    h2_libco_complete(task, result);
    co_switch(core->root);
    for (;;) {
    }
}

static h2_libco_result_t h2_libco_suspend(h2_libco_t *core,
                                         bool respect_cancel) {
    h2_libco_task_t *task = core->running;
    co_switch(core->root);
    return respect_cancel && task->cancel_requested
               ? H2_LIBCO_ERR_CANCELLED
               : task->resume_result;
}

static h2_libco_result_t h2_libco_reclaim(h2_libco_t *core,
                                          h2_libco_task_t *task,
                                          int *out_entry_result) {
    if (!h2_libco_guard_valid(task)) {
        core->faulted = true;
        return H2_LIBCO_ERR_STACK_CORRUPT;
    }
    if (out_entry_result != NULL) {
        *out_entry_result = task->entry_result;
    }
    if (task->stack_allocation != NULL) {
        core->config.free(core->config.user, task->stack_allocation);
        task->stack_allocation = NULL;
        task->stack = NULL;
        task->context = NULL;
    }
    task->state = H2_LIBCO_TASK_JOINED;
    task->joiner = NULL;
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_create(const h2_libco_config_t *config,
                                  h2_libco_t **out_core) {
    h2_libco_t *core;
    if (config == NULL || out_core == NULL || *out_core != NULL ||
        config->alloc == NULL || config->free == NULL ||
        config->now_ms == NULL || s_active_core != NULL) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    core = config->alloc(config->user, sizeof(*core));
    if (core == NULL) {
        return H2_LIBCO_ERR_NO_MEMORY;
    }
    memset(core, 0, sizeof(*core));
    core->config = *config;
    core->root = co_active();
    h2_libco_internal_init_pal_apis(core);
    *out_core = core;
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_destroy(h2_libco_t **core_ptr) {
    h2_libco_t *core;
    h2_libco_task_t *task;
    if (core_ptr == NULL || *core_ptr == NULL) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    core = *core_ptr;
    if (!h2_libco_internal_root_context(core) || core->scheduling) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    if (core->live_pal_objects != 0u) {
        return H2_LIBCO_ERR_BUSY;
    }
    for (task = core->tasks; task != NULL; task = task->all_next) {
        if (task->state != H2_LIBCO_TASK_JOINED) {
            return H2_LIBCO_ERR_BUSY;
        }
    }
    task = core->tasks;
    while (task != NULL) {
        h2_libco_task_t *next = task->all_next;
        core->config.free(core->config.user, task);
        task = next;
    }
    h2_libco_free_fn_t free_fn = core->config.free;
    void *user = core->config.user;
    free_fn(user, core);
    *core_ptr = NULL;
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_task_start(
    h2_libco_t *core,
    const h2_libco_task_options_t *options,
    h2_libco_task_entry_fn_t entry,
    void *user,
    h2_libco_task_t **out_task) {
    size_t stack_size = options == NULL || options->stack_size == 0u
                            ? H2_LIBCO_DEFAULT_STACK_SIZE
                            : options->stack_size;
    size_t context_overhead = 0u;
    size_t context_size;
    size_t allocation_overhead;
    h2_libco_task_t *task;
    uint8_t *raw;
    uintptr_t aligned;
    uint64_t guard;
    if (core == NULL || entry == NULL || out_task == NULL ||
        *out_task != NULL || !h2_libco_valid_context(core)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    if (core->faulted) {
        return H2_LIBCO_ERR_STACK_CORRUPT;
    }
#if defined(__EMSCRIPTEN__)
    context_overhead = h2_libco_emscripten_context_overhead();
#endif
    if (context_overhead > SIZE_MAX - stack_size) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    context_size = stack_size + context_overhead;
    allocation_overhead = H2_LIBCO_STACK_ALIGNMENT - 1u +
                          2u * sizeof(uint64_t);
    if (stack_size < H2_LIBCO_MIN_STACK_SIZE ||
        stack_size % H2_LIBCO_STACK_ALIGNMENT != 0u ||
        context_size > UINT_MAX ||
        context_size > SIZE_MAX - allocation_overhead ||
        !h2_libco_is_power_of_two(H2_LIBCO_STACK_ALIGNMENT)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    task = core->config.alloc(core->config.user, sizeof(*task));
    if (task == NULL) {
        return H2_LIBCO_ERR_NO_MEMORY;
    }
    memset(task, 0, sizeof(*task));
    raw = core->config.alloc(core->config.user,
                             context_size + allocation_overhead);
    if (raw == NULL) {
        core->config.free(core->config.user, task);
        return H2_LIBCO_ERR_NO_MEMORY;
    }
    aligned = ((uintptr_t)(raw + sizeof(uint64_t)) +
               H2_LIBCO_STACK_ALIGNMENT - 1u) &
              ~(uintptr_t)(H2_LIBCO_STACK_ALIGNMENT - 1u);
    task->owner = core;
    task->entry = entry;
    task->user = user;
    task->stack_allocation = raw;
    task->stack = (uint8_t *)aligned;
    task->context_size = context_size;
    task->stack_size = stack_size;
    task->state = H2_LIBCO_TASK_READY;
    guard = H2_LIBCO_GUARD_LOW;
    memcpy(task->stack - sizeof(guard), &guard, sizeof(guard));
    guard = H2_LIBCO_GUARD_HIGH;
    memcpy(task->stack + context_size, &guard, sizeof(guard));
    task->context = co_derive(task->stack, (unsigned int)context_size,
                              h2_libco_task_trampoline);
    if (task->context == NULL) {
        core->config.free(core->config.user, raw);
        core->config.free(core->config.user, task);
        return H2_LIBCO_ERR_NO_MEMORY;
    }
    task->all_next = core->tasks;
    core->tasks = task;
    h2_libco_ready_push(core, task);
    *out_task = task;
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_task_cancel(h2_libco_t *core,
                                       h2_libco_task_t *task) {
    if (!h2_libco_task_owned(core, task) || !h2_libco_valid_context(core)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    if (task->state == H2_LIBCO_TASK_COMPLETE ||
        task->state == H2_LIBCO_TASK_JOINED) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    if (task->cancel_requested) {
        return H2_LIBCO_OK;
    }
    task->cancel_requested = true;
    if (task->state == H2_LIBCO_TASK_WAITING) {
        h2_libco_detach_join(task);
        task->state = H2_LIBCO_TASK_READY;
        task->wait_kind = H2_LIBCO_WAIT_NONE;
        task->deadline_set = false;
        task->resume_result = H2_LIBCO_ERR_CANCELLED;
        h2_libco_ready_push(core, task);
    }
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_task_join(h2_libco_t *core,
                                     h2_libco_task_t *task,
                                     int *out_entry_result) {
    if (!h2_libco_task_owned(core, task) || !h2_libco_valid_context(core)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    if (task->state == H2_LIBCO_TASK_JOINED) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    if (h2_libco_internal_task_context(core)) {
        h2_libco_task_t *current = core->running;
        h2_libco_task_t *cursor;
        h2_libco_result_t result;
        if (current == task || task->joiner != NULL) {
            return H2_LIBCO_ERR_INVALID_STATE;
        }
        for (cursor = task; cursor != NULL;) {
            if (cursor == current) {
                return H2_LIBCO_ERR_INVALID_STATE;
            }
            if (cursor->wait_kind != H2_LIBCO_WAIT_JOIN) {
                break;
            }
            cursor = cursor->join_target;
        }
        if (task->state != H2_LIBCO_TASK_COMPLETE) {
            current->state = H2_LIBCO_TASK_WAITING;
            current->wait_kind = H2_LIBCO_WAIT_JOIN;
            current->join_target = task;
            current->resume_result = H2_LIBCO_OK;
            task->joiner = current;
            result = h2_libco_suspend(core, true);
            h2_libco_detach_join(current);
            if (result != H2_LIBCO_OK) {
                return result;
            }
        }
    } else if (task->state != H2_LIBCO_TASK_COMPLETE) {
        return H2_LIBCO_ERR_BUSY;
    }
    if (task->state != H2_LIBCO_TASK_COMPLETE) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    return h2_libco_reclaim(core, task, out_entry_result);
}

h2_libco_result_t h2_libco_yield(h2_libco_t *core) {
    h2_libco_task_t *task;
    if (!h2_libco_internal_task_context(core)) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    task = core->running;
    if (task->cancel_requested) {
        return H2_LIBCO_ERR_CANCELLED;
    }
    task->state = H2_LIBCO_TASK_READY;
    task->resume_result = H2_LIBCO_OK;
    h2_libco_ready_push(core, task);
    return h2_libco_suspend(core, true);
}

static void h2_libco_expire_waiters(h2_libco_t *core) {
    uint64_t now = core->config.now_ms(core->config.user);
    for (;;) {
        h2_libco_task_t *candidate = NULL;
        for (h2_libco_task_t *task = core->tasks; task != NULL;
             task = task->all_next) {
            if (task->state == H2_LIBCO_TASK_WAITING &&
                task->wait_kind == H2_LIBCO_WAIT_KEY && task->deadline_set &&
                now >= task->deadline_ms &&
                (candidate == NULL || h2_libco_wait_order_before(
                                          task->wait_order,
                                          candidate->wait_order))) {
                candidate = task;
            }
        }
        if (candidate == NULL) {
            return;
        }
        candidate->state = H2_LIBCO_TASK_READY;
        candidate->wait_kind = H2_LIBCO_WAIT_NONE;
        candidate->deadline_set = false;
        candidate->resume_result = H2_LIBCO_ERR_TIMEOUT;
        h2_libco_ready_push(core, candidate);
    }
}

static bool h2_libco_next_deadline(const h2_libco_t *core,
                                   uint64_t *out_deadline_ms) {
    bool found = false;
    uint64_t deadline = UINT64_MAX;
    for (const h2_libco_task_t *task = core->tasks; task != NULL;
         task = task->all_next) {
        if (task->state == H2_LIBCO_TASK_WAITING && task->deadline_set &&
            (!found || task->deadline_ms < deadline)) {
            found = true;
            deadline = task->deadline_ms;
        }
    }
    if (found && out_deadline_ms != NULL) {
        *out_deadline_ms = deadline;
    }
    return found;
}

h2_libco_result_t h2_libco_schedule(h2_libco_t *core,
                                    size_t work_budget,
                                    size_t *out_resumed) {
    size_t snapshot = 0u;
    size_t resumed = 0u;
    h2_libco_result_t guard_result;
    if (out_resumed != NULL) {
        *out_resumed = 0u;
    }
    if (core == NULL || !h2_libco_internal_root_context(core) ||
        core->scheduling) {
        return H2_LIBCO_ERR_INVALID_STATE;
    }
    if (work_budget == 0u) {
        return H2_LIBCO_OK;
    }
    guard_result = h2_libco_check_guards(core);
    if (guard_result != H2_LIBCO_OK) {
        return guard_result;
    }
    core->scheduling = true;
    if (core->config.poll_external != NULL) {
        core->callback_active = true;
        core->external_poll_active = true;
        h2_libco_result_t poll_result =
            core->config.poll_external(core->config.user, core);
        core->external_poll_active = false;
        core->callback_active = false;
        if (poll_result != H2_LIBCO_OK) {
            core->scheduling = false;
            return H2_LIBCO_ERR_EXTERNAL;
        }
    }
    h2_libco_expire_waiters(core);
    for (h2_libco_task_t *task = core->ready_head; task != NULL;
         task = task->ready_next) {
        ++snapshot;
    }
    if (snapshot > work_budget) {
        snapshot = work_budget;
    }
    if (snapshot == 0u) {
        uint64_t deadline_ms = 0u;
        bool has_deadline = h2_libco_next_deadline(core, &deadline_ms);
        if (core->config.idle != NULL) {
            core->callback_active = true;
            core->config.idle(core->config.user, has_deadline ? 1 : 0,
                              deadline_ms);
            core->callback_active = false;
        }
        core->scheduling = false;
        return H2_LIBCO_OK;
    }
    while (resumed < snapshot) {
        h2_libco_task_t *task = h2_libco_ready_pop(core);
        if (task == NULL) {
            break;
        }
        if (task->cancel_requested && !task->started) {
            h2_libco_complete(task, H2_LIBCO_ERR_CANCELLED);
        } else {
            task->started = true;
            task->state = H2_LIBCO_TASK_RUNNING;
            core->running = task;
            s_active_core = core;
            s_active_task = task;
            co_switch(task->context);
            s_active_task = NULL;
            s_active_core = NULL;
            core->running = NULL;
        }
        ++resumed;
        if (h2_libco_check_guards(core) != H2_LIBCO_OK) {
            break;
        }
    }
    core->scheduling = false;
    if (out_resumed != NULL) {
        *out_resumed = resumed;
    }
    return core->faulted ? H2_LIBCO_ERR_STACK_CORRUPT : H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_wait(h2_libco_t *core,
                                uintptr_t wait_key,
                                uint32_t timeout_ms) {
    h2_libco_task_t *task;
    uint64_t now;
    if (!h2_libco_internal_task_context(core) || wait_key == 0u) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    task = core->running;
    if (task->cancel_requested) {
        return H2_LIBCO_ERR_CANCELLED;
    }
    if (timeout_ms == 0u) {
        return H2_LIBCO_ERR_TIMEOUT;
    }
    task->state = H2_LIBCO_TASK_WAITING;
    task->wait_kind = H2_LIBCO_WAIT_KEY;
    task->wait_key = wait_key;
    task->wait_order = ++core->next_wait_order;
    task->resume_result = H2_LIBCO_WOKEN;
    task->deadline_set = timeout_ms != H2_LIBCO_WAIT_FOREVER;
    if (task->deadline_set) {
        now = core->config.now_ms(core->config.user);
        task->deadline_ms = UINT64_MAX - now < timeout_ms
                                ? UINT64_MAX
                                : now + timeout_ms;
    }
    return h2_libco_suspend(core, true);
}

h2_libco_result_t h2_libco_internal_wait_deferred_cancel(
    h2_libco_t *core, uintptr_t wait_key) {
    h2_libco_task_t *task;
    if (!h2_libco_internal_task_context(core) || wait_key == 0u) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    task = core->running;
    task->state = H2_LIBCO_TASK_WAITING;
    task->wait_kind = H2_LIBCO_WAIT_KEY;
    task->wait_key = wait_key;
    task->wait_order = ++core->next_wait_order;
    task->resume_result = H2_LIBCO_WOKEN;
    task->deadline_set = false;
    return h2_libco_suspend(core, false);
}

h2_libco_result_t h2_libco_internal_wake_one(
    h2_libco_t *core, uintptr_t wait_key, h2_libco_task_t **out_task) {
    h2_libco_task_t *candidate = NULL;
    if (out_task != NULL) {
        *out_task = NULL;
    }
    if (core == NULL || wait_key == 0u || !h2_libco_valid_context(core)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    for (h2_libco_task_t *task = core->tasks; task != NULL;
         task = task->all_next) {
        if (task->state == H2_LIBCO_TASK_WAITING &&
            task->wait_kind == H2_LIBCO_WAIT_KEY &&
            task->wait_key == wait_key &&
            (candidate == NULL || h2_libco_wait_order_before(
                                      task->wait_order,
                                      candidate->wait_order))) {
            candidate = task;
        }
    }
    if (candidate != NULL) {
        candidate->state = H2_LIBCO_TASK_READY;
        candidate->wait_kind = H2_LIBCO_WAIT_NONE;
        candidate->deadline_set = false;
        candidate->resume_result = H2_LIBCO_WOKEN;
        h2_libco_ready_push(core, candidate);
    }
    if (out_task != NULL) {
        *out_task = candidate;
    }
    return H2_LIBCO_OK;
}

h2_libco_result_t h2_libco_wake(h2_libco_t *core,
                                uintptr_t wait_key,
                                size_t max_waiters,
                                size_t *out_woken) {
    size_t woken = 0u;
    if (out_woken != NULL) {
        *out_woken = 0u;
    }
    bool external_poll_context =
        core != NULL && core->external_poll_active &&
        s_active_core == NULL && co_active() == core->root;
    if (core == NULL || wait_key == 0u ||
        (!h2_libco_valid_context(core) && !external_poll_context)) {
        return H2_LIBCO_ERR_INVALID_ARG;
    }
    if (max_waiters == 0u) {
        return H2_LIBCO_OK;
    }
    /* Repeatedly select the earliest matching wait operation. */
    while (woken < max_waiters) {
        h2_libco_task_t *candidate = NULL;
        for (h2_libco_task_t *task = core->tasks; task != NULL;
             task = task->all_next) {
            if (task->state == H2_LIBCO_TASK_WAITING &&
                task->wait_kind == H2_LIBCO_WAIT_KEY &&
                task->wait_key == wait_key) {
                if (candidate == NULL || h2_libco_wait_order_before(
                                             task->wait_order,
                                             candidate->wait_order)) {
                    candidate = task;
                }
            }
        }
        if (candidate == NULL) {
            break;
        }
        candidate->state = H2_LIBCO_TASK_READY;
        candidate->wait_kind = H2_LIBCO_WAIT_NONE;
        candidate->deadline_set = false;
        candidate->resume_result = H2_LIBCO_WOKEN;
        h2_libco_ready_push(core, candidate);
        ++woken;
    }
    if (out_woken != NULL) {
        *out_woken = woken;
    }
    return H2_LIBCO_OK;
}

h2_pal_result_t h2_libco_internal_to_pal(h2_libco_result_t result) {
    switch (result) {
    case H2_LIBCO_OK:
    case H2_LIBCO_WOKEN:
        return H2_PAL_OK;
    case H2_LIBCO_ERR_INVALID_ARG:
        return H2_PAL_ERR_INVALID_ARG;
    case H2_LIBCO_ERR_NO_MEMORY:
        return H2_PAL_ERR_NO_MEMORY;
    case H2_LIBCO_ERR_BUSY:
        return H2_PAL_ERR_BUSY;
    case H2_LIBCO_ERR_INVALID_STATE:
        return H2_PAL_ERR_INVALID_STATE;
    case H2_LIBCO_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case H2_LIBCO_ERR_CANCELLED:
        return H2_PAL_EXIT;
    case H2_LIBCO_ERR_STACK_CORRUPT:
    case H2_LIBCO_ERR_EXTERNAL:
    default:
        return H2_PAL_ERR_TASK;
    }
}
