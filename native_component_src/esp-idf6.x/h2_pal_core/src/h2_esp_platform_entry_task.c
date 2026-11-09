#include "h2_esp_platform_entry_task.h"

#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

typedef struct h2_esp_entry_context {
  h2_esp_platform_entry_task_fn entry;
  void* user;
  h2_esp_platform_entry_task_fn after_entry;
  void* after_entry_user;
  h2_esp_task_stack_region_t stack_region;
} h2_esp_entry_context_t;

static void entry_task(void* raw) {
  const h2_esp_entry_context_t context = *(h2_esp_entry_context_t*)raw;
  heap_caps_free(raw);
  context.entry(context.user);
  if (context.after_entry != NULL) {
    context.after_entry(context.after_entry_user);
  }
  if (context.stack_region == H2_ESP_TASK_STACK_PSRAM) {
    vTaskDeleteWithCaps(NULL);
  } else {
    vTaskDelete(NULL);
  }
}

h2_pal_result_t h2_esp_platform_start_entry_task(
    const char* name, h2_esp_platform_entry_task_fn entry, void* user,
    h2_esp_platform_entry_task_fn after_entry, void* after_entry_user) {
  if (name == NULL || name[0] == '\0' || entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_esp_task_policy_t policy = {0};
  h2_pal_result_t rc = h2_esp_platform_task_reserve_policy(name, &policy);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  const unsigned int caps =
      MALLOC_CAP_8BIT |
      (policy.stack_region == H2_ESP_TASK_STACK_PSRAM ? MALLOC_CAP_SPIRAM
                                                      : MALLOC_CAP_INTERNAL);
  h2_esp_entry_context_t* context = heap_caps_malloc(sizeof(*context), caps);
  if (context == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  *context = (h2_esp_entry_context_t){
      .entry = entry,
      .user = user,
      .after_entry = after_entry,
      .after_entry_user = after_entry_user,
      .stack_region = policy.stack_region,
  };
  const BaseType_t core = policy.core == H2_ESP_TASK_CORE_ANY
                              ? tskNO_AFFINITY
                              : (BaseType_t)policy.core;
  const uint32_t stack_size =
      policy.min_stack_size < 4096u ? 4096u : policy.min_stack_size;
  BaseType_t result;
  if (policy.stack_region == H2_ESP_TASK_STACK_PSRAM) {
    result = xTaskCreatePinnedToCoreWithCaps(
        entry_task, name, stack_size, context, (UBaseType_t)policy.priority,
        NULL, core, caps);
  } else {
    result = xTaskCreatePinnedToCore(entry_task, name, stack_size, context,
                                     (UBaseType_t)policy.priority, NULL, core);
  }
  if (result != pdPASS) {
    heap_caps_free(context);
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}
