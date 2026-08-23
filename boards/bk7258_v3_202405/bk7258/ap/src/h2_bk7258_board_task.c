#include "h2_bk7258_board.h"

#ifdef H2_BK7258_BOARD_TASK_HOST_TEST
#include "h2_bk7258_board_task_test_sdk.h"
#else
#include "os/os.h"
#include "os/mem.h"
#endif

#define H2_BK7258_ENTRY_TASK_STACK_SIZE (24u * 1024u)

typedef struct h2_bk7258_board_entry_task_context {
    h2_bk7258_board_entry_task_fn entry;
    void *user;
} h2_bk7258_board_entry_task_context_t;

static void entry_task(beken_thread_arg_t raw) {
    h2_bk7258_board_entry_task_context_t *context =
        (h2_bk7258_board_entry_task_context_t *)raw;
    h2_bk7258_board_entry_task_fn entry = context->entry;
    void *user = context->user;
    os_free(context);
    entry(user);
    (void)h2_bk7258_board_runtime_deinit();
    rtos_delete_thread(NULL);
}

h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char *name,
    h2_bk7258_board_entry_task_fn entry,
    void *user) {
    if (name == NULL || name[0] == '\0' || entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_bk7258_board_entry_task_context_t *context =
        os_malloc(sizeof(*context));
    if (context == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    context->entry = entry;
    context->user = user;

    beken_thread_t thread = NULL;
    if (rtos_create_thread(
            &thread,
            BEKEN_DEFAULT_WORKER_PRIORITY,
            name,
            entry_task,
            H2_BK7258_ENTRY_TASK_STACK_SIZE,
            (beken_thread_arg_t)context) != kNoErr) {
        os_free(context);
        return H2_PAL_ERR_TASK;
    }
    return H2_PAL_OK;
}
