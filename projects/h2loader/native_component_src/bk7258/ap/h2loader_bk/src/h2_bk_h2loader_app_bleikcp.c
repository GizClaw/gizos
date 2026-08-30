#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"
#include "h2loader_app_task_names.h"

#include "driver/flash.h"
#include "mbedtls/sha256.h"
#include "os/os.h"
#include "h2_loader_app_client.h"
#include "h2_loader_ble.h"
#include "h2_loader_boot.h"

#include <stdio.h>
#include <string.h>

#define H2_BK_FLASH_SECTOR_SIZE 4096u

typedef struct h2_bk_h2loader_app_ble {
    h2_runtime_t *runtime;
    h2_loader_app_client_config_t client_config;
    h2_loader_ble_service_t *service;
    h2_pal_mutex_t *operation_mutex;
    char active_version[H2_PAL_FIRMWARE_VERSION_MAX];
} h2_bk_h2loader_app_ble_t;

static h2_bk_h2loader_app_ble_t s_ble;
static int s_started;
static mbedtls_sha256_context s_app_sha;

static int app_digest_start(void *user) {
    (void)user;
    mbedtls_sha256_init(&s_app_sha);
    return mbedtls_sha256_starts(&s_app_sha, 0) == 0
        ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int app_digest_update(void *user, const uint8_t *data, size_t len) {
    (void)user;
    return mbedtls_sha256_update(&s_app_sha, data, len) == 0
        ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int app_digest_finish(void *user, uint8_t out_digest[32]) {
    (void)user;
    return mbedtls_sha256_finish(&s_app_sha, out_digest) == 0
        ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static void app_digest_abort(void *user) {
    (void)user;
    mbedtls_sha256_free(&s_app_sha);
}

static void app_digest_hex(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; ++i) {
        out[i * 2u] = hex[digest[i] >> 4u];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

static int current_app_identity(
    h2_runtime_t *runtime,
    const char *version,
    h2_loader_image_identity_t *out_identity) {
    const h2_loader_image_reader_api_t *reader = h2_bk_h2loader_image_reader();
    uint8_t buffer[4096];
    uint8_t digest[32];
    uint64_t image_size = 0u;
    uint64_t offset = 0u;
    int rc;
    if (runtime == NULL || version == NULL || out_identity == NULL ||
        reader == NULL || reader->vtable == NULL ||
        reader->vtable->get_capacity == NULL || reader->vtable->read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = reader->vtable->get_capacity(
        reader->user, H2_BK_H2LOADER_APP_PARTITION_ID, &image_size);
    if (rc != H2_PAL_OK || image_size == 0u) return rc;
    rc = app_digest_start(NULL);
    while (rc == H2_PAL_OK && offset < image_size) {
        size_t take = image_size - offset > sizeof(buffer)
            ? sizeof(buffer) : (size_t)(image_size - offset);
        rc = reader->vtable->read(reader->user,
            H2_BK_H2LOADER_APP_PARTITION_ID, offset, buffer, take);
        if (rc == H2_PAL_OK) rc = app_digest_update(NULL, buffer, take);
        offset += take;
    }
    if (rc == H2_PAL_OK) rc = app_digest_finish(NULL, digest);
    if (rc != H2_PAL_OK) {
        app_digest_abort(NULL);
        return rc;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    out_identity->format = 1u;
    out_identity->role = H2_LOADER_IMAGE_ROLE_APP;
    out_identity->image_size = image_size;
    app_digest_hex(digest, out_identity->image_sha256);
    (void)snprintf(out_identity->board, sizeof(out_identity->board),
        "%s", runtime->board);
    (void)snprintf(out_identity->target, sizeof(out_identity->target),
        "%s", runtime->target);
    (void)snprintf(out_identity->version, sizeof(out_identity->version),
        "%s", version);
    return H2_PAL_OK;
}

static uint64_t app_now_ms(void *user) {
    const h2_pal_time_api_t *time = user;
    uint64_t value = 0u;
    return h2_pal_time_get_monotonic_ms(time, &value) == H2_PAL_OK
        ? value : 0u;
}

static void app_sleep_ms(void *user, uint32_t delay_ms) {
    (void)user;
    rtos_delay_milliseconds(delay_ms);
}

int h2_bk_h2loader_prepare_app_operation(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->sync == NULL || runtime->mem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_ble.runtime != NULL) {
        return s_ble.runtime == runtime && s_ble.operation_mutex != NULL
            ? H2_PAL_OK
            : H2_PAL_ERR_INVALID_STATE;
    }

    memset(&s_ble, 0, sizeof(s_ble));
    s_ble.runtime = runtime;
    const h2_pal_mutex_config_t mutex_config = {
        .name = "h2loader-app-operation",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    int rc = h2_pal_mutex_create(
        runtime->sync, &mutex_config, &s_ble.operation_mutex);
    if (rc != H2_PAL_OK) {
        memset(&s_ble, 0, sizeof(s_ble));
    }
    return rc;
}

int h2_bk_h2loader_app_operation_lock(void) {
    if (s_ble.runtime == NULL || s_ble.operation_mutex == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_pal_mutex_lock(s_ble.runtime->sync, s_ble.operation_mutex);
}

void h2_bk_h2loader_app_operation_unlock(void) {
    if (s_ble.runtime != NULL && s_ble.operation_mutex != NULL) {
        (void)h2_pal_mutex_unlock(s_ble.runtime->sync, s_ble.operation_mutex);
    }
}

static h2_pal_result_t coredump_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    (void)user;
    if (partition_id != H2_BK_H2LOADER_COREDUMP_ADDR || out_partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = H2_BK_H2LOADER_COREDUMP_ADDR;
    out_partition->flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
        H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
    out_partition->size = H2_BK_H2LOADER_COREDUMP_SIZE;
    out_partition->erase_block_size = H2_BK_FLASH_SECTOR_SIZE;
    out_partition->write_alignment = 1u;
    (void)snprintf(out_partition->name, sizeof(out_partition->name), "coredump");
    return H2_PAL_OK;
}

static h2_pal_result_t coredump_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    (void)user;
    if (partition_id != H2_BK_H2LOADER_COREDUMP_ADDR || data == NULL ||
        offset > H2_BK_H2LOADER_COREDUMP_SIZE ||
        len > H2_BK_H2LOADER_COREDUMP_SIZE - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return bk_flash_read_bytes(
        H2_BK_H2LOADER_COREDUMP_ADDR + (uint32_t)offset,
        data,
        (uint32_t)len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t coredump_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    (void)user;
    if (partition_id != H2_BK_H2LOADER_COREDUMP_ADDR ||
        offset > H2_BK_H2LOADER_COREDUMP_SIZE ||
        len > H2_BK_H2LOADER_COREDUMP_SIZE - offset ||
        (offset % H2_BK_FLASH_SECTOR_SIZE) != 0u ||
        (len % H2_BK_FLASH_SECTOR_SIZE) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    flash_protect_type_t protect = bk_flash_get_protect_type();
    int rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    for (uint64_t off = offset; rc == 0 && off < offset + len;
         off += H2_BK_FLASH_SECTOR_SIZE) {
        rc = bk_flash_erase_sector(H2_BK_H2LOADER_COREDUMP_ADDR + (uint32_t)off);
    }
    (void)bk_flash_set_protect_type(protect);
    return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t disk_unsupported(void *user, uint32_t partition_id) {
    (void)user;
    (void)partition_id;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t coredump_write(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    const void *data,
    size_t len) {
    (void)user;
    (void)partition_id;
    (void)offset;
    (void)data;
    (void)len;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_disk_vtable_t s_coredump_disk_vtable = {
    .get_partition = coredump_get_partition,
    .read = coredump_read,
    .erase = coredump_erase,
    .write = coredump_write,
    .flush = disk_unsupported,
};

static const h2_pal_disk_api_t s_coredump_disk = {
    .vtable = &s_coredump_disk_vtable,
};

int h2_bk_h2loader_init_app_client(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities,
    h2_loader_app_client_t *client) {
    if (runtime == NULL || active_name == NULL || active_name[0] == '\0' ||
        client == NULL ||
        (hardware_capabilities & H2_LOADER_CAPABILITY_UART) == 0u ||
        (hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_bk_h2loader_prepare_app_operation(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_firmware_info_t firmware_info;
    rc = h2_pal_firmware_info_get_current(
        runtime->firmware_info, &firmware_info);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)snprintf(
        s_ble.active_version,
        sizeof(s_ble.active_version),
        "%s",
        firmware_info.version);
    const h2_loader_app_client_config_t config = {
        .pref = runtime->pref,
        .power = h2_bk_h2loader_app_power_api(runtime->pref),
        .allocator = runtime->mem,
        .disk = &s_coredump_disk,
        .fs = runtime->fs,
        .http = runtime->http,
        .wifi = runtime->wifi_sta,
        .wifi_settings = runtime->wifi_settings,
        .digest = {
            .start = app_digest_start,
            .update = app_digest_update,
            .finish = app_digest_finish,
            .abort = app_digest_abort,
        },
        .operation_sync = runtime->sync,
        .operation_mutex = s_ble.operation_mutex,
        .wifi_operation_sync = runtime->sync,
        .wifi_operation_mutex = s_ble.operation_mutex,
        .board = runtime->board,
        .target = runtime->target,
        .chip = runtime->chip,
        .hardware_capabilities = hardware_capabilities,
        .h2loader_partition_id = H2_BK_H2LOADER_PRIMARY_PARTITION_ID,
        .app_partition_id = 2u,
        .coredump_partition_id = H2_BK_H2LOADER_COREDUMP_ADDR,
        .clock_user = (void *)runtime->time,
        .now_ms = app_now_ms,
        .sleep_ms = app_sleep_ms,
    };
    h2_loader_app_client_config_t complete = config;
    rc = current_app_identity(
        runtime, s_ble.active_version, &complete.active_identity);
    return rc == H2_PAL_OK
        ? h2_loader_app_client_init(client, &complete) : rc;
}

static int handle_ble_session(void *user, h2_bleikcp_t *stream, uint16_t conn_handle) {
    h2_bk_h2loader_app_ble_t *ble = user;
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
        .stack_size = 8192u,
    };
    rc = h2_loader_app_client_start_return_console(&console);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_loader_app_client_join_return_console(&client);
}

static int h2_bk_h2loader_start_app_ble_with_mode(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities,
    h2_loader_ble_advertising_mode_t advertising_mode) {
    h2_loader_app_client_t client;

    if (runtime == NULL || active_name == NULL || active_name[0] == '\0' ||
        (hardware_capabilities & H2_LOADER_CAPABILITY_BLE) == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int rc = h2_bk_h2loader_init_app_client(
        runtime, active_name, hardware_capabilities, &client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    s_ble.client_config = client.config;
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
        .advertising_mode = advertising_mode,
        .handler = handle_ble_session,
        .handler_user = &s_ble,
    };
    rc = h2_loader_ble_service_open(&service, &s_ble.service);
    if (rc == H2_PAL_OK) {
        s_started = 1;
    }
    return rc;
}

int h2_bk_h2loader_start_app_ble(
    h2_runtime_t *runtime,
    const char *active_name) {
    return h2_bk_h2loader_start_app_ble_with_mode(
        runtime,
        active_name,
        H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
        H2_LOADER_BLE_ADVERTISING_LEGACY);
}

int h2_bk_h2loader_start_app_ble_with_capabilities(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities) {
    return h2_bk_h2loader_start_app_ble_with_mode(
        runtime, active_name, hardware_capabilities,
        H2_LOADER_BLE_ADVERTISING_LEGACY);
}

int h2_bk_h2loader_start_app_ble_extended(
    h2_runtime_t *runtime,
    const char *active_name) {
    return h2_bk_h2loader_start_app_ble_with_mode(
        runtime,
        active_name,
        H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
        H2_LOADER_BLE_ADVERTISING_EXTENDED);
}

int h2_bk_h2loader_start_app_ble_extended_with_capabilities(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities) {
    return h2_bk_h2loader_start_app_ble_with_mode(
        runtime, active_name, hardware_capabilities,
        H2_LOADER_BLE_ADVERTISING_EXTENDED);
}

int h2_bk_h2loader_pause_app_ble_advertising(void) {
    if (!s_started || s_ble.service == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_pause_advertising(s_ble.service);
}

int h2_bk_h2loader_resume_app_ble_advertising(void) {
    if (!s_started || s_ble.service == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_resume_advertising(s_ble.service);
}

int h2_bk_h2loader_advertise_app_ble_service(
    const h2_pal_ble_uuid_t *service_uuid) {
    if (!s_started || s_ble.service == NULL || service_uuid == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_loader_ble_service_set_additional_advertised_services(
        s_ble.service, service_uuid, 1u);
}
