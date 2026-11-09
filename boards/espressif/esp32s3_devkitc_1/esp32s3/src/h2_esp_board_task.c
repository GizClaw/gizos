#include "h2_esp_board.h"

#include "h2_esp_platform_entry_task.h"

static void entry_finished(void* unused) {
  (void)unused;
  (void)h2_esp_board_runtime_deinit();
}

h2_pal_result_t h2_esp_board_start_entry_task(const char* name,
                                              h2_esp_board_entry_task_fn entry,
                                              void* user) {
  return h2_esp_platform_start_entry_task(name, entry, user, entry_finished,
                                          NULL);
}
