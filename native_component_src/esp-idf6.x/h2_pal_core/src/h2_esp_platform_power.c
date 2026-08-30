#include "h2_esp_platform_core.h"
#include "h2_esp_platform_pref_migration.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

#define H2_ESP_POWER_PARTITION_LOADER 1u
#define H2_ESP_POWER_PARTITION_APP 2u
#define H2_ESP_POWER_DEEP_SLEEP_WAKE_US (1000ULL * 1000ULL)
#define H2_ESP_POWER_OTA_TASK_STACK_DEPTH 4096u

typedef enum h2_esp_power_ota_op {
    H2_ESP_POWER_OTA_GET_RUNNING = 1,
    H2_ESP_POWER_OTA_GET_NEXT,
    H2_ESP_POWER_OTA_SET_NEXT,
    H2_ESP_POWER_OTA_CONFIRM_RUNNING,
    H2_ESP_POWER_REBOOT,
} h2_esp_power_ota_op_t;

typedef struct h2_esp_power_ota_call {
    h2_esp_power_ota_op_t op;
    const esp_partition_t *partition;
    uint32_t partition_id;
    esp_err_t result;
} h2_esp_power_ota_call_t;

static int s_h2_esp_power_hold;
static StaticSemaphore_t s_ota_mutex_storage;
static SemaphoreHandle_t s_ota_mutex;
static portMUX_TYPE s_ota_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

h2_pal_result_t __attribute__((weak)) h2_esp_platform_power_before_reboot(uint32_t reason) {
    (void)reason;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_capabilities(void *user, h2_pal_power_capabilities_t *out_capabilities);
static h2_pal_result_t power_get_boot_info(void *user, h2_pal_power_boot_info_t *out_info);
static h2_pal_result_t power_get_state(void *user, h2_pal_power_state_t *out_state);
static h2_pal_result_t power_list_boot_partitions(
    void *user,
    h2_pal_power_boot_partition_cb_t cb,
    void *cb_user);
static h2_pal_result_t power_get_running_boot_partition(void *user, h2_pal_power_boot_partition_t *out_partition);
static h2_pal_result_t power_get_next_boot_partition(void *user, h2_pal_power_boot_partition_t *out_partition);
static h2_pal_result_t power_set_next_boot_partition(void *user, uint32_t partition_id);
static h2_pal_result_t power_set_hold(void *user, int enabled);
static h2_pal_result_t power_get_hold(void *user, h2_pal_power_hold_state_t *out_state);
static h2_pal_result_t power_reboot(void *user, uint32_t reason);
static h2_pal_result_t power_deep_sleep(void *user, uint32_t reason);

static const h2_pal_power_vtable_t s_power_vtable = {
    .get_capabilities = power_get_capabilities,
    .get_boot_info = power_get_boot_info,
    .get_state = power_get_state,
    .list_boot_partitions = power_list_boot_partitions,
    .get_running_boot_partition = power_get_running_boot_partition,
    .get_next_boot_partition = power_get_next_boot_partition,
    .set_next_boot_partition = power_set_next_boot_partition,
    .set_hold = power_set_hold,
    .get_hold = power_get_hold,
    .reboot = power_reboot,
    .deep_sleep = power_deep_sleep,
};

static const h2_pal_power_api_t s_default_power_api = {
    .user = NULL,
    .vtable = &s_power_vtable,
};

static const esp_partition_t *partition_for_id(uint32_t id) {
    if (id == H2_ESP_POWER_PARTITION_LOADER) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    if (id == H2_ESP_POWER_PARTITION_APP) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    }
    return NULL;
}

static uint32_t id_for_partition(const esp_partition_t *partition) {
    if (partition == NULL) {
        return 0u;
    }
    if (partition->type == ESP_PARTITION_TYPE_APP && partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return H2_ESP_POWER_PARTITION_LOADER;
    }
    if (partition->type == ESP_PARTITION_TYPE_APP && partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return H2_ESP_POWER_PARTITION_APP;
    }
    return 0u;
}

static void IRAM_ATTR power_ota_safe_callback(void *context) {
    h2_esp_power_ota_call_t *call = (h2_esp_power_ota_call_t *)context;
    switch (call->op) {
        case H2_ESP_POWER_OTA_GET_RUNNING:
            call->partition = esp_ota_get_running_partition();
            call->result = call->partition != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
            break;
        case H2_ESP_POWER_OTA_GET_NEXT:
            call->partition = esp_ota_get_boot_partition();
            call->result = call->partition != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
            break;
        case H2_ESP_POWER_OTA_SET_NEXT:
            call->partition = partition_for_id(call->partition_id);
            if (call->partition == NULL) {
                call->result = ESP_ERR_NOT_FOUND;
                break;
            }
            {
                const esp_partition_t *running =
                    esp_ota_get_running_partition();
                /* Re-selecting the running OTA slot changes its otadata state
                 * back to NEW. A same-slot reboot must leave a confirmed image
                 * VALID so callers can distinguish it from a real slot switch. */
                call->result = running != NULL &&
                        running->address == call->partition->address
                    ? ESP_OK
                    : esp_ota_set_boot_partition(call->partition);
            }
            break;
        case H2_ESP_POWER_OTA_CONFIRM_RUNNING:
            call->result = esp_ota_mark_app_valid_cancel_rollback();
            break;
        case H2_ESP_POWER_REBOOT:
            esp_restart();
            call->result = ESP_FAIL;
            break;
        default:
            call->result = ESP_ERR_INVALID_ARG;
            break;
    }
}

static SemaphoreHandle_t power_ota_mutex(void) {
    portENTER_CRITICAL(&s_ota_mutex_init_lock);
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutexStatic(&s_ota_mutex_storage);
    }
    portEXIT_CRITICAL(&s_ota_mutex_init_lock);
    return s_ota_mutex;
}

static h2_esp_power_ota_call_t *power_ota_call_alloc(h2_esp_power_ota_op_t op) {
    h2_esp_power_ota_call_t *call = (h2_esp_power_ota_call_t *)heap_caps_calloc(
        1u,
        sizeof(*call),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (call != NULL) {
        call->op = op;
    }
    return call;
}

static h2_pal_result_t power_ota_call(h2_esp_power_ota_call_t *call) {
    SemaphoreHandle_t mutex;
    h2_pal_result_t rc;
    if (call == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    mutex = power_ota_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TASK;
    }
    rc = h2_esp_platform_safe_call(
        power_ota_safe_callback,
        call,
        sizeof(*call),
        H2_ESP_POWER_OTA_TASK_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    if (rc == H2_PAL_OK && call->result != ESP_OK) {
        rc = call->result == ESP_ERR_NOT_FOUND ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_IO;
    }
    return rc;
}

h2_pal_result_t h2_esp_platform_confirm_running_app(void) {
    h2_esp_power_ota_call_t *call =
        power_ota_call_alloc(H2_ESP_POWER_OTA_CONFIRM_RUNNING);
    h2_pal_result_t rc = power_ota_call(call);
    heap_caps_free(call);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_platform_pref_finalize_migration();
    }
    return rc;
}

static void fill_boot_partition(
    const esp_partition_t *partition,
    uint32_t id,
    h2_pal_power_boot_partition_t *out_partition) {
    size_t len;

    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = id;
    out_partition->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE;
    if (id == H2_ESP_POWER_PARTITION_LOADER) {
        out_partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY;
    } else if (id == H2_ESP_POWER_PARTITION_APP) {
        out_partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_APP;
    }
    if (partition != NULL) {
        len = strnlen(partition->label, sizeof(partition->label));
        if (len >= sizeof(out_partition->name)) {
            len = sizeof(out_partition->name) - 1u;
        }
        memcpy(out_partition->name, partition->label, len);
        out_partition->name[len] = '\0';
    }
}

static h2_pal_result_t power_get_capabilities(void *user, h2_pal_power_capabilities_t *out_capabilities) {
    (void)user;
    if (out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_capabilities->flags =
        H2_PAL_POWER_CAPABILITY_HOLD |
        H2_PAL_POWER_CAPABILITY_REBOOT |
        H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
        H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS |
        H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION |
        H2_PAL_POWER_CAPABILITY_RESET_REASON;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_boot_info(void *user, h2_pal_power_boot_info_t *out_info) {
    esp_reset_reason_t reason;

    (void)user;
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->source = H2_PAL_POWER_BOOT_SOURCE_UNKNOWN;
    out_info->previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_UNKNOWN;
    reason = esp_reset_reason();
    switch (reason) {
    case ESP_RST_POWERON:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_POWER_ON;
        break;
    case ESP_RST_SW:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_SOFTWARE;
        break;
    case ESP_RST_PANIC:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_PANIC;
        break;
    case ESP_RST_DEEPSLEEP:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_DEEP_SLEEP;
        break;
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_WATCHDOG;
        break;
    case ESP_RST_BROWNOUT:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_BROWNOUT;
        break;
    default:
        out_info->reset_reason = H2_PAL_POWER_RESET_REASON_UNKNOWN;
        break;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_state(void *user, h2_pal_power_state_t *out_state) {
    (void)user;
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_state = H2_PAL_POWER_STATE_RUNNING;
    return H2_PAL_OK;
}

static h2_pal_result_t power_list_boot_partitions(
    void *user,
    h2_pal_power_boot_partition_cb_t cb,
    void *cb_user) {
    const esp_partition_t *loader;
    const esp_partition_t *app;
    h2_pal_power_boot_partition_t partition;
    h2_pal_result_t rc;

    (void)user;
    if (cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    loader = partition_for_id(H2_ESP_POWER_PARTITION_LOADER);
    app = partition_for_id(H2_ESP_POWER_PARTITION_APP);
    if (loader != NULL) {
        fill_boot_partition(loader, H2_ESP_POWER_PARTITION_LOADER, &partition);
        rc = cb(cb_user, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (app != NULL) {
        fill_boot_partition(app, H2_ESP_POWER_PARTITION_APP, &partition);
        rc = cb(cb_user, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_running_boot_partition(void *user, h2_pal_power_boot_partition_t *out_partition) {
    h2_esp_power_ota_call_t *call;
    const esp_partition_t *partition = NULL;
    uint32_t id;

    (void)user;
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    call = power_ota_call_alloc(H2_ESP_POWER_OTA_GET_RUNNING);
    h2_pal_result_t rc = power_ota_call(call);
    if (rc == H2_PAL_OK) {
        partition = call->partition;
    }
    heap_caps_free(call);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    id = id_for_partition(partition);
    if (partition == NULL || id == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    fill_boot_partition(partition, id, out_partition);
    out_partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_next_boot_partition(void *user, h2_pal_power_boot_partition_t *out_partition) {
    h2_esp_power_ota_call_t *call;
    const esp_partition_t *partition = NULL;
    uint32_t id;

    (void)user;
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    call = power_ota_call_alloc(H2_ESP_POWER_OTA_GET_NEXT);
    h2_pal_result_t rc = power_ota_call(call);
    if (rc == H2_PAL_OK) {
        partition = call->partition;
    }
    heap_caps_free(call);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    id = id_for_partition(partition);
    if (partition == NULL || id == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    fill_boot_partition(partition, id, out_partition);
    out_partition->flags |= H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT;
    return H2_PAL_OK;
}

static h2_pal_result_t power_set_next_boot_partition(void *user, uint32_t partition_id) {
    h2_esp_power_ota_call_t *call;

    (void)user;
    if (partition_id != H2_ESP_POWER_PARTITION_LOADER &&
        partition_id != H2_ESP_POWER_PARTITION_APP) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    call = power_ota_call_alloc(H2_ESP_POWER_OTA_SET_NEXT);
    if (call != NULL) {
        call->partition_id = partition_id;
    }
    h2_pal_result_t rc = power_ota_call(call);
    heap_caps_free(call);
    return rc;
}

static h2_pal_result_t power_set_hold(void *user, int enabled) {
    (void)user;
    s_h2_esp_power_hold = enabled ? 1 : 0;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_hold(void *user, h2_pal_power_hold_state_t *out_state) {
    (void)user;
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_state->enabled = s_h2_esp_power_hold;
    return H2_PAL_OK;
}

static h2_pal_result_t power_reboot(void *user, uint32_t reason) {
    h2_esp_power_ota_call_t *call;

    (void)user;
    h2_pal_result_t rc = h2_esp_platform_power_before_reboot(reason);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    call = power_ota_call_alloc(H2_ESP_POWER_REBOOT);
    rc = power_ota_call(call);
    heap_caps_free(call);
    return rc;
}

static h2_pal_result_t power_deep_sleep(void *user, uint32_t reason) {
    esp_err_t err;

    (void)user;
    (void)reason;
    err = esp_sleep_enable_timer_wakeup(H2_ESP_POWER_DEEP_SLEEP_WAKE_US);
    if (err != ESP_OK) {
        return H2_PAL_ERR_IO;
    }
    esp_deep_sleep_start();
    return H2_PAL_OK;
}

const h2_pal_power_api_t *h2_esp_platform_power_api(void) {
    return &s_default_power_api;
}
