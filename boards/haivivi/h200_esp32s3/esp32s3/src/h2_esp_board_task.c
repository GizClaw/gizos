#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
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
  const h2_pal_power_api_t* power = h2_esp_board_power_api();
  h2_pal_power_hold_state_t hold = {0};
  h2_pal_result_t rc = h2_pal_power_set_hold(power, 1);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_power_get_hold(power, &hold);
  }
  if (rc != H2_PAL_OK || hold.enabled == 0) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  return h2_esp_platform_start_entry_task(name, entry, user, after_entry, NULL);
}
