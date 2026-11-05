#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_zero_esp_board_internal.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <string.h>

#define H2_ZERO_ESP_ENTRY_TASK_STACK_SIZE 65536u
#define H2_ZERO_ESP_H106_TASK_NAME "h106/entry"
#define H2_ZERO_ESP_H106_TASK_PRIORITY (tskIDLE_PRIORITY + 4u)
#define H2_ZERO_ESP_H106_TASK_CORE tskNO_AFFINITY

typedef struct h2_zero_esp_entry_task_context {
  h2_esp_board_entry_task_fn entry;
  void* user;
} h2_zero_esp_entry_task_context_t;

static void entry_task(void* raw) {
  h2_zero_esp_entry_task_context_t* context =
      (h2_zero_esp_entry_task_context_t*)raw;
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
  h2_esp_board_arena_init();
  if (h2_esp_zero_esp_display_validate_fixed_memory() != H2_DISPLAY_OK) {
    return H2_PAL_ERR_NO_MEMORY;
  }
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
  h2_zero_esp_entry_task_context_t* context =
      heap_caps_malloc(sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (context == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  context->entry = entry;
  context->user = user;
  const int h106_task = strcmp(name, H2_ZERO_ESP_H106_TASK_NAME) == 0;
  BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
      entry_task, name, H2_ZERO_ESP_ENTRY_TASK_STACK_SIZE, context,
      h106_task ? H2_ZERO_ESP_H106_TASK_PRIORITY : tskIDLE_PRIORITY + 4u, NULL,
      h106_task ? H2_ZERO_ESP_H106_TASK_CORE : tskNO_AFFINITY,
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
