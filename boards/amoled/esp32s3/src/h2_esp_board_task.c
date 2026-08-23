#include "h2_esp_board.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define H2_AMOLED_ENTRY_TASK_STACK_SIZE 65536u

typedef struct h2_esp_board_entry_task_context {
    h2_esp_board_entry_task_fn entry;
    void *user;
} h2_esp_board_entry_task_context_t;

static void entry_task(void *raw) {
    h2_esp_board_entry_task_context_t *context =
        (h2_esp_board_entry_task_context_t *)raw;
    h2_esp_board_entry_task_fn entry = context->entry;
    void *user = context->user;
    heap_caps_free(context);
    entry(user);
    (void)h2_esp_board_runtime_deinit();
    vTaskDeleteWithCaps(NULL);
}
h2_pal_result_t h2_esp_board_start_entry_task(
    const char *name,
    h2_esp_board_entry_task_fn entry,
    void *user) {
    if (name == NULL || name[0] == '\0' || entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    h2_esp_board_entry_task_context_t *context =
        heap_caps_malloc(sizeof(*context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    context->entry = entry;
    context->user = user;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        entry_task, name, H2_AMOLED_ENTRY_TASK_STACK_SIZE, context,
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
