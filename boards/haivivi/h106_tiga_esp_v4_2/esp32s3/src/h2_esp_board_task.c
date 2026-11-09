#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_tiga_board_internal.h"
#include "h2_esp_platform_entry_task.h"

static void after_entry(void* user) {
  (void)user;
  (void)h2_esp_board_runtime_deinit();
}

h2_pal_result_t h2_esp_board_start_entry_task(const char* name,
                                              h2_esp_board_entry_task_fn entry,
                                              void* user) {
  if (name == NULL || name[0] == '\0' || entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_esp_board_arena_init();
  if (h2_esp_tiga_display_validate_fixed_memory() != H2_DISPLAY_OK) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return h2_esp_platform_start_entry_task(name, entry, user, after_entry, NULL);
}
