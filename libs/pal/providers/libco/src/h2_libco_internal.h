#ifndef H2_LIBCO_INTERNAL_H
#define H2_LIBCO_INTERNAL_H

#include "h2_libco.h"
#include "libco.h"

#include <stdbool.h>

typedef enum h2_libco_task_state {
    H2_LIBCO_TASK_READY = 0,
    H2_LIBCO_TASK_RUNNING,
    H2_LIBCO_TASK_WAITING,
    H2_LIBCO_TASK_COMPLETE,
    H2_LIBCO_TASK_JOINED,
} h2_libco_task_state_t;

typedef enum h2_libco_wait_kind {
    H2_LIBCO_WAIT_NONE = 0,
    H2_LIBCO_WAIT_KEY,
    H2_LIBCO_WAIT_JOIN,
} h2_libco_wait_kind_t;

typedef struct h2_libco_pal_task h2_libco_pal_task_t;

struct h2_libco_task {
    h2_libco_t *owner;
    struct h2_libco_task *all_next;
    struct h2_libco_task *ready_next;
    struct h2_libco_task *joiner;
    struct h2_libco_task *join_target;
    h2_libco_task_entry_fn_t entry;
    void *user;
    void *stack_allocation;
    uint8_t *stack;
    size_t context_size;
    size_t stack_size;
    cothread_t context;
    h2_libco_task_state_t state;
    h2_libco_wait_kind_t wait_kind;
    h2_libco_result_t resume_result;
    uintptr_t wait_key;
    uintptr_t granted_key;
    uint64_t deadline_ms;
    uint64_t wait_order;
    int entry_result;
    bool deadline_set;
    bool cancel_requested;
    bool queued;
    bool started;
};

struct h2_libco {
    h2_libco_config_t config;
    cothread_t root;
    h2_libco_task_t *tasks;
    h2_libco_task_t *ready_head;
    h2_libco_task_t *ready_tail;
    h2_libco_task_t *running;
    h2_pal_task_api_t task_api;
    h2_pal_time_api_t time_api;
    h2_pal_queue_api_t queue_api;
    h2_pal_sync_api_t sync_api;
    bool scheduling;
    bool callback_active;
    bool external_poll_active;
    bool faulted;
    uint64_t next_wait_order;
    size_t live_pal_objects;
};

bool h2_libco_internal_root_context(const h2_libco_t *core);
bool h2_libco_internal_task_context(const h2_libco_t *core);
h2_libco_task_t *h2_libco_internal_current_task(h2_libco_t *core);
h2_libco_result_t h2_libco_internal_wait_deferred_cancel(
    h2_libco_t *core, uintptr_t wait_key);
h2_libco_result_t h2_libco_internal_wake_one(
    h2_libco_t *core, uintptr_t wait_key, h2_libco_task_t **out_task);
h2_pal_result_t h2_libco_internal_to_pal(h2_libco_result_t result);
void h2_libco_internal_init_pal_apis(h2_libco_t *core);
const h2_pal_time_vtable_t *h2_libco_internal_time_vtable(void);
const h2_pal_queue_vtable_t *h2_libco_internal_queue_vtable(void);
const h2_pal_sync_vtable_t *h2_libco_internal_sync_vtable(void);

#endif
