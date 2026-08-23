#include "h2_bk7258_board.h"
#include "h2_bk7258_board_task_test_sdk.h"

#include <assert.h>
#include <stdlib.h>

typedef struct test_state {
    int fail_alloc;
    int fail_start;
    size_t allocations;
    size_t frees;
    size_t starts;
    size_t deletes;
    size_t entries;
    size_t teardowns;
    beken_thread_function_t task_entry;
    beken_thread_arg_t task_user;
} test_state_t;

static test_state_t s_state;

void *os_malloc(size_t size) {
    s_state.allocations += 1u;
    return s_state.fail_alloc ? NULL : malloc(size);
}

void os_free(void *ptr) {
    s_state.frees += 1u;
    free(ptr);
}

int rtos_create_thread(
    beken_thread_t *out_thread,
    int priority,
    const char *name,
    beken_thread_function_t entry,
    uint32_t stack_size,
    beken_thread_arg_t user) {
    assert(out_thread != NULL);
    assert(priority == BEKEN_DEFAULT_WORKER_PRIORITY);
    assert(name != NULL);
    assert(stack_size == 24u * 1024u);
    s_state.starts += 1u;
    s_state.task_entry = entry;
    s_state.task_user = user;
    *out_thread = (beken_thread_t)(uintptr_t)1u;
    return s_state.fail_start ? -1 : kNoErr;
}

void rtos_delete_thread(beken_thread_t *thread) {
    assert(thread == NULL);
    s_state.deletes += 1u;
}

h2_pal_result_t h2_bk7258_board_runtime_deinit(void) {
    assert(s_state.entries == 1u);
    assert(s_state.deletes == 0u);
    s_state.teardowns += 1u;
    return H2_PAL_OK;
}

static void board_entry(void *user) {
    assert(user == &s_state);
    assert(s_state.frees == 1u);
    s_state.entries += 1u;
}

static void reset_state(void) {
    s_state = (test_state_t){ 0 };
}

int main(void) {
    assert(h2_bk7258_board_start_entry_task(NULL, board_entry, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_bk7258_board_start_entry_task("", board_entry, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_bk7258_board_start_entry_task("test", NULL, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);

    reset_state();
    s_state.fail_alloc = 1;
    assert(h2_bk7258_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(s_state.allocations == 1u && s_state.frees == 0u);

    reset_state();
    s_state.fail_start = 1;
    assert(h2_bk7258_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_ERR_TASK);
    assert(s_state.allocations == 1u && s_state.frees == 1u);

    reset_state();
    assert(h2_bk7258_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_OK);
    assert(s_state.allocations == 1u && s_state.frees == 0u);
    assert(s_state.task_entry != NULL && s_state.task_user != 0u);
    s_state.task_entry(s_state.task_user);
    assert(s_state.frees == 1u && s_state.entries == 1u &&
           s_state.teardowns == 1u && s_state.deletes == 1u);
    return 0;
}
