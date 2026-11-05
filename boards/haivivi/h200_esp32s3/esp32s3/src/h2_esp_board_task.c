#include "h2_esp_board.h"
#include "h2_esp_board_private.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define H2_H200_ENTRY_TASK_STACK_SIZE 65536u

typedef struct entry_context {
  h2_esp_board_entry_task_fn entry;
  void* user;
} entry_context_t;

static void entry_task(void* raw) {
  entry_context_t* context = (entry_context_t*)raw;
  h2_esp_board_entry_task_fn entry = context->entry;
  void* user = context->user;
  heap_caps_free(context);
  entry(user);
  (void)h2_esp_board_runtime_deinit();
  vTaskDeleteWithCaps(NULL);
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
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
  entry_context_t* context =
      heap_caps_malloc(sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (context == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  context->entry = entry;
  context->user = user;
  BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
      entry_task, name, H2_H200_ENTRY_TASK_STACK_SIZE, context,
      tskIDLE_PRIORITY + 4u, NULL, tskNO_AFFINITY,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (result != pdPASS) {
    heap_caps_free(context);
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
#else
  (void)user;
  return H2_PAL_ERR_UNSUPPORTED;
#endif
}
