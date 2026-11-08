#include "h2_bk7258_board.h"
#include "h2_bk_platform_entry_task.h"

static void after_entry(void* user) {
  (void)user;
  (void)h2_bk7258_board_runtime_deinit();
}

h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char* name, h2_bk7258_board_entry_task_fn entry, void* user) {
  return h2_bk_platform_start_entry_task(name, entry, user, after_entry, NULL);
}
