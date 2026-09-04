#include "h2_esp_h2loader_ble.h"
#include "h2loader_app_task_names.h"

#include "h2_esp_h2loader_iostreamikcp.h"
#include "h2_esp_h2loader_app_commands_preflight.h"
#include "h2_esp_platform_wifi_activity.h"
#include "h2loader_bleikcp_internal.h"
#include "h2_loader_app_client.h"
#include "h2_loader_ble.h"
#include "h2_loader_boot.h"

#include "esp_mac.h"

#include <stdio.h>
#include <string.h>

#define H2_ESP_H2LOADER_APP_COMMAND_STACK_SIZE (32u * 1024u)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define H2_ESP_H2LOADER_APP_BLE_START_ATTEMPTS 6u

/* Private link contract with h2_h2loader_runtime. */
void h2_esp_h2loader_run_with_command_service_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *config,
    const h2loader_app_command_service_api_t *command_service);

typedef struct h2_esp_h2loader_app_ble {
    h2_runtime_t *runtime;
    h2_loader_app_client_config_t client_config;
    h2_loader_ble_service_t *service;
    h2_pal_mutex_t *operation_mutex;
    char device_uid[13];
} h2_esp_h2loader_app_ble_t;

static h2_loader_app_client_t s_serial_client;
static h2_esp_h2loader_app_ble_t s_ble;
static int s_serial_started;

static void app_wifi_activity(void *user, bool active) {
    (void)user;
    if (s_ble.service != NULL) {
        (void)h2_loader_ble_service_request_advertising_paused(
            s_ble.service, active);
    }
}

static uint64_t app_now_ms(void *user) {
    const h2_pal_time_api_t *time = user;
    uint64_t value = 0u;
    return h2_pal_time_get_monotonic_ms(time, &value) == H2_PAL_OK
        ? value : 0u;
}

static void app_sleep_ms(void *user, uint32_t delay_ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

void h2_esp_h2loader_run_with_ble_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *config) {
    if (config == NULL) {
        h2_esp_h2loader_run_with_config(runtime, NULL);
        return;
    }
    h2_esp_h2loader_run_with_command_service_config(
        runtime, config, h2loader_bleikcp_command_service());
}

static int handle_ble_session(
    void *user,
    h2_bleikcp_t *stream,
    uint16_t conn_handle) {
    h2_esp_h2loader_app_ble_t *ble = user;
    h2_loader_app_client_t client;
    (void)conn_handle;
    if (ble == NULL || ble->runtime == NULL || stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_loader_app_client_init(&client, &ble->client_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_loader_app_client_return_console_config_t console = {
        .client = &client,
        .task = ble->runtime->task,
        .read_user = stream,
        .read_byte = h2_loader_ble_app_read_byte,
        .write_user = stream,
        .write = h2_loader_ble_app_write,
        .task_name = h2loader_app_command_task_name,
        .stack_size = H2_ESP_H2LOADER_APP_COMMAND_STACK_SIZE,
    };
    rc = h2_loader_app_client_start_return_console(&console);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_loader_app_client_join_return_console(&client);
}

int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *runtime_config,
    const h2_esp_h2loader_app_commands_config_t *config) {
    if (runtime_config == NULL || runtime_config->task == NULL ||
        runtime_config->sync == NULL || runtime_config->mem == NULL ||
        runtime_config->pref == NULL || runtime_config->power == NULL ||
        runtime_config->firmware_info == NULL || runtime_config->time == NULL ||
        runtime_config->fs == NULL || runtime_config->disk == NULL ||
        runtime_config->http == NULL || runtime_config->wifi_sta == NULL ||
        runtime_config->board == NULL || runtime_config->target == NULL ||
        runtime_config->chip == NULL || config == NULL ||
        config->active_name == NULL || config->active_name[0] == '\0' ||
        (config->hardware_capabilities & H2_LOADER_CAPABILITY_UART) == 0u ||
        (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_serial_started || s_ble.operation_mutex != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    memset(&s_ble, 0, sizeof(s_ble));
    int cleanup_rc;
    uint8_t ble_mac[6];
    h2_esp_h2loader_app_commands_preflight_t preflight;
    int rc = h2_esp_h2loader_app_commands_preflight(
        runtime_config, &preflight);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    s_ble.operation_mutex = preflight.operation_mutex;
    rc = esp_read_mac(ble_mac, ESP_MAC_BT);
    if (rc != ESP_OK) {
        rc = H2_PAL_ERR_IO;
        goto cleanup_mutex;
    }
    (void)snprintf(
        s_ble.device_uid,
        sizeof(s_ble.device_uid),
        "%02x%02x%02x%02x%02x%02x",
        ble_mac[0], ble_mac[1], ble_mac[2], ble_mac[3], ble_mac[4], ble_mac[5]);
    s_ble.client_config = (h2_loader_app_client_config_t){
        .pref = runtime_config->pref,
        .power = runtime_config->power,
        .allocator = runtime_config->mem,
        .disk = runtime_config->disk,
        .fs = runtime_config->fs,
        .http = runtime_config->http,
        .wifi = runtime_config->wifi_sta,
        .wifi_settings = runtime_config->wifi_settings,
        .digest = h2_esp_h2loader_digest_api(),
        .operation_sync = runtime_config->sync,
        .operation_mutex = s_ble.operation_mutex,
        .board = runtime_config->board,
        .target = runtime_config->target,
        .chip = runtime_config->chip,
        .device_uid = s_ble.device_uid,
        .hardware_capabilities = config->hardware_capabilities,
        .memory_stats = {
            .read = h2_esp_h2loader_memory_stats_read,
        },
        .h2loader_partition_id = config->h2loader_partition_id,
        .app_partition_id = preflight.app_partition_id,
        .coredump_partition_id = config->coredump_partition_id,
        .clock_user = (void *)runtime_config->time,
        .now_ms = app_now_ms,
        .sleep_ms = app_sleep_ms,
    };
    rc = h2_esp_h2loader_current_image_identity(
        H2_LOADER_IMAGE_ROLE_APP,
        runtime_config->board,
        runtime_config->target,
        config->active_version != NULL && config->active_version[0] != '\0'
            ? config->active_version : preflight.firmware_info.version,
        &s_ble.client_config.active_identity);
    if (rc != H2_PAL_OK) goto cleanup_mutex;
    rc = h2_loader_app_client_init(&s_serial_client, &s_ble.client_config);
    if (rc != H2_PAL_OK) {
        goto cleanup_mutex;
    }
    rc = h2_esp_h2loader_app_iostreamikcp_start(
        &s_serial_client,
        runtime_config->task,
        runtime_config->mem,
        H2_ESP_H2LOADER_APP_COMMAND_STACK_SIZE);
    if (rc != H2_PAL_OK) {
        goto cleanup_mutex;
    }
    s_serial_started = 1;
    return H2_PAL_OK;

cleanup_mutex:
    cleanup_rc = h2_pal_mutex_destroy(
        runtime_config->sync, s_ble.operation_mutex);
    if (cleanup_rc != H2_PAL_OK) {
        return cleanup_rc;
    }
    memset(&s_serial_client, 0, sizeof(s_serial_client));
    memset(&s_ble, 0, sizeof(s_ble));
    return rc;
}

int h2_esp_h2loader_app_commands_prepare_serial(
    const h2_runtime_config_t *runtime_config,
    const char *active_name,
    uint32_t h2loader_partition_id,
    uint32_t coredump_partition_id) {
    const h2_esp_h2loader_app_commands_config_t config = {
        .active_name = active_name,
        .hardware_capabilities =
            H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI |
            H2_LOADER_CAPABILITY_BLE,
        .h2loader_partition_id = h2loader_partition_id,
        .coredump_partition_id = coredump_partition_id,
    };
    return h2_esp_h2loader_app_commands_prepare_serial_with_config(
        runtime_config, &config);
}

int h2_esp_h2loader_app_commands_start_with_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_app_commands_config_t *config) {
    if (runtime == NULL || runtime->ble_host == NULL || runtime->task == NULL ||
        runtime->time == NULL || runtime->sync == NULL ||
        runtime->system_event == NULL || runtime->mem == NULL ||
        config == NULL || config->active_name == NULL ||
        config->active_name[0] == '\0' ||
        (config->hardware_capabilities & H2_LOADER_CAPABILITY_BLE) == 0u ||
        (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!s_serial_started || s_ble.service != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((config->active_version != NULL && config->active_version[0] != '\0' &&
         strcmp(s_ble.client_config.active_identity.version,
                config->active_version) != 0) ||
        s_ble.client_config.hardware_capabilities !=
            config->hardware_capabilities ||
        s_ble.client_config.h2loader_partition_id !=
            config->h2loader_partition_id ||
        s_ble.client_config.coredump_partition_id !=
            config->coredump_partition_id) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_ble.runtime = runtime;
    const h2_loader_ble_service_config_t service = {
        .api = {
            .ble = runtime->ble_host,
            .task = runtime->task,
            .time = runtime->time,
            .sync = runtime->sync,
            .system_event = runtime->system_event,
            .allocator = runtime->mem,
        },
        .board = runtime->board,
        .capabilities = s_ble.client_config.hardware_capabilities,
        .advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
        .handler = handle_ble_session,
        .handler_user = &s_ble,
    };
    uint32_t retry_ms = 250u;
    for (uint32_t attempt = 1u;
         attempt <= H2_ESP_H2LOADER_APP_BLE_START_ATTEMPTS;
         ++attempt) {
        int rc = h2_loader_ble_service_open(&service, &s_ble.service);
        if (rc == H2_PAL_OK) {
            h2_esp_platform_wifi_set_activity_observer(
                app_wifi_activity, NULL);
            return H2_PAL_OK;
        }
        s_ble.service = NULL;
        if (attempt == H2_ESP_H2LOADER_APP_BLE_START_ATTEMPTS) {
            printf(
                "H2_ESP_H2LOADER_APP_BLE_START_FAIL rc=%d attempts=%u\n",
                rc,
                (unsigned)attempt);
            s_ble.runtime = NULL;
            return rc;
        }
        printf(
            "H2_ESP_H2LOADER_APP_BLE_START_RETRY rc=%d attempt=%u delay_ms=%u\n",
            rc,
            (unsigned)attempt,
            (unsigned)retry_ms);
        rc = h2_pal_time_sleep_ms(runtime->time, retry_ms);
        if (rc != H2_PAL_OK) {
            s_ble.runtime = NULL;
            return rc;
        }
        if (retry_ms < 5000u) {
            retry_ms *= 2u;
            if (retry_ms > 5000u) retry_ms = 5000u;
        }
    }
    s_ble.runtime = NULL;
    return H2_PAL_ERR_UNAVAILABLE;
}

int h2_esp_h2loader_app_commands_start(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t h2loader_partition_id,
    uint32_t coredump_partition_id) {
    const h2_esp_h2loader_app_commands_config_t config = {
        .active_name = active_name,
        .hardware_capabilities =
            H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI |
            H2_LOADER_CAPABILITY_BLE,
        .h2loader_partition_id = h2loader_partition_id,
        .coredump_partition_id = coredump_partition_id,
    };
    return h2_esp_h2loader_app_commands_start_with_config(runtime, &config);
}

int h2_esp_h2loader_app_commands_pause_ble_advertising(void) {
    if (s_ble.service == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_pause_advertising(s_ble.service);
}

int h2_esp_h2loader_app_commands_resume_ble_advertising(void) {
    if (s_ble.service == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_resume_advertising(s_ble.service);
}

int h2_esp_h2loader_app_commands_advertise_ble_service(
    const h2_pal_ble_uuid_t *service_uuid) {
    if (s_ble.service == NULL || service_uuid == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_set_additional_advertised_services(
        s_ble.service, service_uuid, 1u);
}
