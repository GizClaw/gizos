#include "h2_esp_board.h"
#include "h2_esp_board_task_test_sdk.h"

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
    TaskFunction_t task_entry;
    void *task_user;
} test_state_t;

static test_state_t s_state;

void *heap_caps_malloc(size_t size, uint32_t capabilities) {
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_state.allocations += 1u;
    return s_state.fail_alloc ? NULL : malloc(size);
}

void heap_caps_free(void *ptr) {
    s_state.frees += 1u;
    free(ptr);
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry,
    const char *name,
    uint32_t stack_size,
    void *user,
    uint32_t priority,
    void *out_task,
    int core_id,
    uint32_t capabilities) {
    assert(name != NULL);
    assert(stack_size == 65536u);
    assert(priority == tskIDLE_PRIORITY + 4u);
    assert(out_task == NULL);
    assert(core_id == tskNO_AFFINITY);
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_state.starts += 1u;
    s_state.task_entry = entry;
    s_state.task_user = user;
    return s_state.fail_start ? 0 : pdPASS;
}

void vTaskDeleteWithCaps(void *task) {
    assert(task == NULL);
    s_state.deletes += 1u;
}

h2_pal_result_t h2_esp_board_runtime_deinit(void) {
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
    assert(h2_esp_board_start_entry_task(NULL, board_entry, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_board_start_entry_task("", board_entry, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_board_start_entry_task("test", NULL, &s_state) ==
           H2_PAL_ERR_INVALID_ARG);

    reset_state();
    s_state.fail_alloc = 1;
    assert(h2_esp_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(s_state.allocations == 1u && s_state.frees == 0u);

    reset_state();
    s_state.fail_start = 1;
    assert(h2_esp_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_ERR_TASK);
    assert(s_state.allocations == 1u && s_state.frees == 1u);

    reset_state();
    assert(h2_esp_board_start_entry_task("test", board_entry, &s_state) ==
           H2_PAL_OK);
    assert(s_state.allocations == 1u && s_state.frees == 0u);
    assert(s_state.task_entry != NULL && s_state.task_user != NULL);
    s_state.task_entry(s_state.task_user);
    assert(s_state.frees == 1u && s_state.entries == 1u &&
           s_state.teardowns == 1u && s_state.deletes == 1u);
    return 0;
}
