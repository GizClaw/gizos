#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM && \
    (!defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) || \
     !CONFIG_SPIRAM_XIP_FROM_PSRAM)
#error "ESP targets with PSRAM must enable CONFIG_SPIRAM_XIP_FROM_PSRAM"
#endif

struct h2_pal_task {
    TaskHandle_t task;
    SemaphoreHandle_t done;
    h2_pal_task_entry_t entry;
    void *ctx;
    int stack_with_caps;
};

typedef struct h2_esp_task_policy {
    const char *name;
    UBaseType_t priority;
    BaseType_t core_id;
    uint32_t stack_caps;
} h2_esp_task_policy_t;

#define H2_ESP_TASK_STACK_INTERNAL 0u
#define H2_ESP_TASK_STACK_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define H2_ESP_TASK_CONTROL_CORE 0
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define H2_ESP_TASK_MEDIA_CORE 1
#else
#define H2_ESP_TASK_MEDIA_CORE H2_ESP_TASK_CONTROL_CORE
#endif
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define H2_ESP_TASK_STACK_EXTERNAL (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define H2_ESP_TASK_STACK_EXTERNAL H2_ESP_TASK_STACK_INTERNAL
#endif

static const h2_esp_task_policy_t s_task_policies[] = {
    {
        .name = "cmd/h2test",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = tskNO_AFFINITY,
        .stack_caps = H2_ESP_TASK_STACK_INTERNAL,
    },
    {
        .name = "h2loader/appcmd",
        .priority = tskIDLE_PRIORITY + 8u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "h2loader/return",
        .priority = tskIDLE_PRIORITY + 8u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "h2loader/blelink",
        .priority = tskIDLE_PRIORITY + 6u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "bleikcp/kcp",
        .priority = tskIDLE_PRIORITY + 7u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "bleikcp/server",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "bleikcp-speed/kcp",
        .priority = tskIDLE_PRIORITY + 7u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "bleikcp-speed/server",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "h2peer/net",
        .priority = tskIDLE_PRIORITY + 7u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "h2peer/udp",
        .priority = tskIDLE_PRIORITY + 7u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
    {
        .name = "aud_play",
        .priority = tskIDLE_PRIORITY + 10u,
        .core_id = H2_ESP_TASK_MEDIA_CORE,
        .stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    },
    {
        .name = "ble_speed_p",
        .priority = tskIDLE_PRIORITY + 4u,
        .core_id = tskNO_AFFINITY,
        .stack_caps = H2_ESP_TASK_STACK_EXTERNAL,
    },
    {
        .name = "ble_speed_c",
        .priority = tskIDLE_PRIORITY + 4u,
        .core_id = tskNO_AFFINITY,
        .stack_caps = H2_ESP_TASK_STACK_EXTERNAL,
    },
    {
        .name = "net/modem_ppp_rx",
        .priority = tskIDLE_PRIORITY + 7u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_EXTERNAL,
    },
    {
        .name = "modem/call_in",
        .priority = tskIDLE_PRIORITY + 6u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_EXTERNAL,
    },
    {
        .name = "modem/gnss_fix",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_EXTERNAL,
    },
    {
        .name = "rt_btn_poll",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = H2_ESP_TASK_CONTROL_CORE,
        .stack_caps = H2_ESP_TASK_STACK_INTERNAL,
    },
    {
        .name = "swdraw",
        .priority = tskIDLE_PRIORITY + 5u,
        .core_id = H2_ESP_TASK_MEDIA_CORE,
        .stack_caps = H2_ESP_TASK_STACK_PSRAM,
    },
};

static const h2_esp_task_policy_t s_default_task_policy = {
    .name = NULL,
    .priority = tskIDLE_PRIORITY + 4u,
    .core_id = tskNO_AFFINITY,
    .stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
};

static const h2_esp_task_policy_t *esp_task_policy_for_name(const char *name) {
    if (name == NULL) {
        return &s_default_task_policy;
    }
    for (size_t i = 0u; i < (sizeof(s_task_policies) / sizeof(s_task_policies[0])); ++i) {
        if (strcmp(name, s_task_policies[i].name) == 0) {
            return &s_task_policies[i];
        }
    }
    return &s_default_task_policy;
}

static void esp_task_trampoline(void *raw) {
    h2_pal_task_t *task = (h2_pal_task_t *)raw;
    const int stack_with_caps = task->stack_with_caps;
    task->entry(task->ctx);
    xSemaphoreGive(task->done);
    if (stack_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static int esp_task_start(void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;

    if (options == NULL || entry == NULL || out_task == NULL) {
        return -1;
    }

    h2_pal_task_t *task = (h2_pal_task_t *)calloc(1u, sizeof(*task));
    if (task == NULL) {
        return -1;
    }
    task->done = xSemaphoreCreateBinary();
    if (task->done == NULL) {
        free(task);
        return -1;
    }
    task->entry = entry;
    task->ctx = ctx;

    const uint32_t stack_size = options->min_stack_size > 0u ? (uint32_t)options->min_stack_size : 4096u;
    const h2_esp_task_policy_t *policy =
        esp_task_policy_for_name(options->name);
    const uint32_t stack_caps = policy->stack_caps;
    BaseType_t ok;
    if (stack_caps == H2_ESP_TASK_STACK_INTERNAL) {
        task->stack_with_caps = 0;
        ok = xTaskCreatePinnedToCore(
            esp_task_trampoline,
            options->name != NULL ? options->name : "h2_task",
            stack_size,
            task,
            policy->priority,
            &task->task,
            policy->core_id);
    } else {
        task->stack_with_caps = 1;
        ok = xTaskCreatePinnedToCoreWithCaps(
            esp_task_trampoline,
            options->name != NULL ? options->name : "h2_task",
            stack_size,
            task,
            policy->priority,
            &task->task,
            policy->core_id,
            stack_caps);
    }
    if (ok != pdPASS) {
        vSemaphoreDelete(task->done);
        free(task);
        return -1;
    }
    printf(
        "H2_PAL_TASK_READY name=%s stack=%s size=%lu priority=%lu core=%ld\n",
        options->name != NULL ? options->name : "h2_task",
        stack_caps == H2_ESP_TASK_STACK_INTERNAL ? "internal" : "psram",
        (unsigned long)stack_size,
        (unsigned long)policy->priority,
        (long)policy->core_id);

    *out_task = task;
    return 0;
}

static int esp_task_join(void *user, h2_pal_task_t *task) {
    (void)user;

    if (task == NULL || task->done == NULL) {
        return -1;
    }
    if (xSemaphoreTake(task->done, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    vSemaphoreDelete(task->done);
    free(task);
    return 0;
}

const h2_pal_task_api_t *h2_esp_platform_task_api(void) {
    static const h2_pal_task_vtable_t vtable = {
        .start = esp_task_start,
        .join = esp_task_join,
    };
    static const h2_pal_task_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
