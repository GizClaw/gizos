#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_loader_boot.h"
#include "h2_loader_command.h"
#include "h2_loader_package.h"
#include "h2_loader_ble.h"
#include "h2loader_bleikcp_internal.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"
#include "driver/flash.h"
#include "driver/wdt.h"
#include "mbedtls/sha256.h"
#include "os/os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string
extern void bk_wdt_force_feed(void);

#define H2_BK_COREDUMP_ADDR H2_BK_H2LOADER_COREDUMP_ADDR
#define H2_BK_COREDUMP_SIZE H2_BK_H2LOADER_COREDUMP_SIZE
#define H2_BK_FLASH_SECTOR_SIZE 4096u
#define H2_BK_STAGE_PATH "/dl/update.tar.zlib"
#define H2_BK_STAGE_PREV_PATH "/dl/update.tar.zlib.prev"
#define H2_BK_STARTUP_RETRY_DELAY_MS 10000u

static h2_pal_fs_api_t s_h2loader_fs;
static h2_runtime_t *s_runtime;
static h2_loader_t s_h2loader;
static h2_loader_command_t s_h2loader_command;
static void *s_h2loader_ble_service;
static h2_pal_mutex_t *s_h2loader_operation_mutex;
static mbedtls_sha256_context s_command_sha;
static int s_h2loader_startup_error;
static char s_h2loader_startup_error_stage[24] = "none";
static int s_h2loader_initialized;
static volatile int s_h2loader_file_points_ready;
static volatile int s_h2loader_mount_in_progress;

static const char *startup_action_name(h2_loader_startup_action_t action) {
    switch (action) {
    case H2_LOADER_STARTUP_ACTION_COMMAND_MODE:
        return "command";
    case H2_LOADER_STARTUP_ACTION_REBOOTING_APP:
        return "rebooting_app";
    case H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER:
        return "rebooting_h2loader";
    default:
        return "unknown";
    }
}

static void on_event(void *user, h2_loader_startup_event_t event, int code) {
    char line[80];

    (void)user;
    (void)bk_wdt_feed();
    bk_wdt_force_feed();
    snprintf(line, sizeof(line), "H2_BK_H2LOADER_EVENT event=%d code=%d\r\n", (int)event, code);
    emergency_uart_write_string(0, line);
}

static void install_progress(
    void *user,
    const h2_bundle_entry_t *entry,
    const h2_bundle_install_stats_t *stats) {
    char line[160];

    (void)user;
    (void)bk_wdt_feed();
    bk_wdt_force_feed();
    if (entry == NULL || stats == NULL) {
        return;
    }
    snprintf(
        line,
        sizeof(line),
        "H2_BK_H2LOADER_INSTALL entry=%lu files=%lu payload=%llu path=%s\r\n",
        (unsigned long)stats->entry_count,
        (unsigned long)stats->file_count,
        (unsigned long long)stats->payload_bytes,
        entry->path);
    emergency_uart_write_string(0, line);
}

static void record_startup_error(const char *stage, int rc) {
    char line[96];

    s_h2loader_startup_error = rc;
    snprintf(
        s_h2loader_startup_error_stage,
        sizeof(s_h2loader_startup_error_stage),
        "%s",
        stage != NULL ? stage : "unknown");
    snprintf(
        line,
        sizeof(line),
        "H2_BK_H2LOADER_STARTUP_ERROR stage=%s rc=%d\r\n",
        s_h2loader_startup_error_stage,
        rc);
    emergency_uart_write_string(0, line);
}

static void clear_startup_error(void) {
    s_h2loader_startup_error = H2_PAL_OK;
    snprintf(
        s_h2loader_startup_error_stage,
        sizeof(s_h2loader_startup_error_stage),
        "none");
}

static int persist_startup_result(int rc) {
    return s_h2loader_initialized ?
        h2_loader_set_last_result(&s_h2loader, rc) : H2_PAL_OK;
}

static int try_mount_file_point(const char *stage, const char *path) {
    int rc;

    rc = h2_bk_h2loader_mount_file_point(NULL, path);
    if (rc == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    record_startup_error(stage, rc);
    return rc;
}

static int try_mount_startup_file_points(void) {
    int rc;

    if (s_h2loader_file_points_ready) {
        clear_startup_error();
        return H2_PAL_OK;
    }
    if (s_h2loader_mount_in_progress) {
        return s_h2loader_startup_error != H2_PAL_OK
            ? s_h2loader_startup_error
            : H2_PAL_ERR_INVALID_STATE;
    }

    s_h2loader_mount_in_progress = 1;
    rc = try_mount_file_point("mount_dl", "/dl");

    if (rc == H2_PAL_OK) {
        rc = try_mount_file_point("mount_data", "/data");
    }
    if (rc == H2_PAL_OK) {
        s_h2loader_file_points_ready = 1;
        clear_startup_error();
    } else {
        s_h2loader_file_points_ready = 0;
    }
    s_h2loader_mount_in_progress = 0;
    return rc;
}

static void wait_forever(void) {
    for (;;) {
        rtos_delay_milliseconds(1000);
    }
}

static int command_digest_start(void *user) {
    (void)user;
    mbedtls_sha256_init(&s_command_sha);
    return mbedtls_sha256_starts(&s_command_sha, 0) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int command_digest_update(void *user, const uint8_t *data, size_t len) {
    (void)user;
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)bk_wdt_feed();
    bk_wdt_force_feed();
    return mbedtls_sha256_update(&s_command_sha, data, len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int command_digest_finish(void *user, uint8_t out_digest[32]) {
    (void)user;
    if (out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return mbedtls_sha256_finish(&s_command_sha, out_digest) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static void command_digest_abort(void *user) {
    (void)user;
    mbedtls_sha256_free(&s_command_sha);
}

static uint64_t command_now_ms(void *user) {
    (void)user;
    return (uint64_t)rtos_get_time();
}

static void command_sleep_ms(void *user, uint32_t delay_ms) {
    (void)user;
    rtos_delay_milliseconds(delay_ms);
}

static h2_pal_result_t coredump_disk_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    (void)user;
    if (partition_id != H2_BK_COREDUMP_ADDR || out_partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = H2_BK_COREDUMP_ADDR;
    out_partition->flags =
        H2_PAL_DISK_PARTITION_FLAG_READABLE |
        H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
    out_partition->size = H2_BK_COREDUMP_SIZE;
    out_partition->erase_block_size = H2_BK_FLASH_SECTOR_SIZE;
    out_partition->write_alignment = 1u;
    snprintf(out_partition->name, sizeof(out_partition->name), "coredump");
    return H2_PAL_OK;
}

static h2_pal_result_t coredump_disk_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    (void)user;
    if (partition_id != H2_BK_COREDUMP_ADDR || data == NULL ||
        offset > H2_BK_COREDUMP_SIZE || len > H2_BK_COREDUMP_SIZE - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return bk_flash_read_bytes(H2_BK_COREDUMP_ADDR + (uint32_t)offset, data, (uint32_t)len) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static h2_pal_result_t coredump_disk_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    flash_protect_type_t protect;
    int rc;

    (void)user;
    if (partition_id != H2_BK_COREDUMP_ADDR ||
        offset > H2_BK_COREDUMP_SIZE ||
        len > H2_BK_COREDUMP_SIZE - offset ||
        (offset % H2_BK_FLASH_SECTOR_SIZE) != 0u ||
        (len % H2_BK_FLASH_SECTOR_SIZE) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    protect = bk_flash_get_protect_type();
    rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    for (uint64_t off = offset; rc == 0 && off < offset + len; off += H2_BK_FLASH_SECTOR_SIZE) {
        rc = bk_flash_erase_sector(H2_BK_COREDUMP_ADDR + (uint32_t)off);
    }
    (void)bk_flash_set_protect_type(protect);
    return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t coredump_disk_unsupported(void *user, uint32_t partition_id) {
    (void)user;
    (void)partition_id;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t coredump_disk_write(
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
    .get_partition = coredump_disk_get_partition,
    .read = coredump_disk_read,
    .erase = coredump_disk_erase,
    .write = coredump_disk_write,
    .flush = coredump_disk_unsupported,
};

static const h2_pal_disk_api_t s_coredump_disk = {
    .vtable = &s_coredump_disk_vtable,
};

static void h2loader_startup_worker(void *user) {
    h2_pal_firmware_info_t firmware_info;
    h2_loader_config_t config = {
        .package = {
            .fs = &s_h2loader_fs,
            .allocator = h2_bk7258_board_psram_allocator(),
            .digest = {
                .start = command_digest_start,
                .update = command_digest_update,
                .finish = command_digest_finish,
                .abort = command_digest_abort,
            },
            .app_entry_path = H2_BK_H2LOADER_APP_ENTRY_PATH,
            .app_writer = h2_bk_h2loader_ota_app_writer(),
            .image_reader = h2_bk_h2loader_image_reader(),
            .image_writer = h2_bk_h2loader_image_writer(),
            .clear_data = h2_bk_h2loader_clear_data,
        },
        .pref = s_runtime->pref,
        .power = h2_bk_h2loader_power_api(),
        .board = "bk7258_v3_202405",
        .target = "bk7258",
        .chip = "bk7258",
        .h2loader_partition_id = H2_BK_H2LOADER_PRIMARY_PARTITION_ID,
        .app_partition_id = H2_BK_H2LOADER_APP_PARTITION_ID,
        .active_identity = {
            .format = 1u,
            .role = H2_LOADER_IMAGE_ROLE_H2LOADER,
            .board = "bk7258_v3_202405",
            .target = "bk7258",
        },
        .confirm_active_image = h2_bk_h2loader_confirm_active_loader,
        .mount_file_point = h2_bk_h2loader_mount_file_point,
        .on_event = on_event,
        .capabilities = H2_LOADER_CAPABILITIES_LOADER,
    };
    h2_loader_command_config_t command_config = {
        .loader = &s_h2loader,
        .fs = &s_h2loader_fs,
        .http = s_runtime->http,
        .wifi = s_runtime->wifi_sta,
        .wifi_settings = s_runtime->wifi_settings,
        .disk = &s_coredump_disk,
        .digest = {
            .start = command_digest_start,
            .update = command_digest_update,
            .finish = command_digest_finish,
            .abort = command_digest_abort,
        },
        .now_ms = command_now_ms,
        .sleep_ms = command_sleep_ms,
        .coredump_partition_id = H2_BK_COREDUMP_ADDR,
    };

    (void)user;

    int rc;
    const h2_pal_mutex_config_t operation_mutex_config = {
        .name = "h2loader-operation",
        .allocator = s_runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    rc = h2_pal_mutex_create(
        s_runtime->sync,
        &operation_mutex_config,
        &s_h2loader_operation_mutex);
    if (rc != H2_PAL_OK) {
        record_startup_error("operation_mutex", rc);
        wait_forever();
    }
    command_config.operation_sync = s_runtime->sync;
    command_config.operation_mutex = s_h2loader_operation_mutex;
    rc = h2_pal_mutex_lock(
        s_runtime->sync,
        s_h2loader_operation_mutex);
    if (rc != H2_PAL_OK) {
        record_startup_error("initialization_lock", rc);
        wait_forever();
    }
    rc = h2_pal_firmware_info_get_current(
        s_runtime->firmware_info,
        &firmware_info);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_mutex_unlock(
            s_runtime->sync,
            s_h2loader_operation_mutex);
        record_startup_error("firmware_info", rc);
        wait_forever();
    }
    (void)snprintf(
        config.active_identity.version,
        sizeof(config.active_identity.version),
        "%s",
        firmware_info.version);

    rc = h2_loader_init(&s_h2loader, &config);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_mutex_unlock(
            s_runtime->sync,
            s_h2loader_operation_mutex);
        record_startup_error("loader_init", rc);
        wait_forever();
    }
    s_h2loader_initialized = 1;
    emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=loader_init rc=0\r\n");
    h2_bundle_installer_set_progress(&s_h2loader.package.installer, install_progress, NULL);
    rc = h2_bk_h2loader_start_loader_iostreamikcp(
        s_runtime,
        &s_h2loader_command,
        &command_config);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_mutex_unlock(
            s_runtime->sync,
            s_h2loader_operation_mutex);
        record_startup_error("iostreamikcp", rc);
        wait_forever();
    }
    emergency_uart_write_string(
        0,
        "H2_BK_H2LOADER_STEP stage=iostreamikcp_start rc=0\r\n");
    rc = h2_pal_mutex_unlock(
        s_runtime->sync,
        s_h2loader_operation_mutex);
    if (rc != H2_PAL_OK) {
        record_startup_error("initialization_unlock", rc);
        wait_forever();
    }

    for (;;) {
        h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
        char line[96];

        rc = h2_pal_mutex_lock(
            s_runtime->sync,
            s_h2loader_operation_mutex);
        if (rc != H2_PAL_OK) {
            record_startup_error("startup_lock", rc);
            wait_forever();
        }
        rc = try_mount_startup_file_points();
        if (rc == H2_PAL_OK) {
            emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=mount rc=0\r\n");

            rc = h2_loader_package_recover_publish(
                &s_h2loader_fs,
                s_runtime->pref,
                H2_BK_STAGE_PATH,
                H2_BK_STAGE_PREV_PATH);
            if (rc != H2_PAL_OK) {
                record_startup_error("publish_recover", rc);
                (void)persist_startup_result(rc);
                (void)h2_pal_mutex_unlock(
                    s_runtime->sync,
                    s_h2loader_operation_mutex);
                rtos_delay_milliseconds(H2_BK_STARTUP_RETRY_DELAY_MS);
                continue;
            }
            emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=publish_recover rc=0\r\n");
        } else {
            record_startup_error("mount", rc);
            (void)persist_startup_result(rc);
            (void)h2_pal_mutex_unlock(
                s_runtime->sync,
                s_h2loader_operation_mutex);
            emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=mount_retry\r\n");
            rtos_delay_milliseconds(H2_BK_STARTUP_RETRY_DELAY_MS);
            continue;
        }

        record_startup_error("startup_begin", H2_PAL_ERR_INVALID_STATE);
        (void)persist_startup_result(H2_PAL_ERR_INVALID_STATE);
        emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=startup_begin rc=0\r\n");
        rc = h2_loader_startup(&s_h2loader, &action);
        if (rc == H2_PAL_OK) {
            clear_startup_error();
            rc = persist_startup_result(H2_PAL_OK);
            if (rc != H2_PAL_OK) {
                record_startup_error("startup_result", rc);
            }
        } else {
            record_startup_error("startup", rc);
            (void)persist_startup_result(rc);
        }
        int unlock_rc = h2_pal_mutex_unlock(
            s_runtime->sync,
            s_h2loader_operation_mutex);
        if (rc == H2_PAL_OK && unlock_rc != H2_PAL_OK) {
            rc = unlock_rc;
        }
        snprintf(
            line,
            sizeof(line),
            "H2_BK_H2LOADER_READY rc=%d action=%s\r\n",
            rc,
            startup_action_name(action));
        emergency_uart_write_string(0, line);
        if (rc == H2_PAL_OK) {
            if (action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE) {
                const h2loader_app_command_service_api_t *command_service =
                    h2loader_bleikcp_command_service();
                if (command_service == NULL) {
                    rc = H2_PAL_ERR_UNSUPPORTED;
                } else if (command_service->open == NULL) {
                    rc = H2_PAL_ERR_INVALID_ARG;
                } else {
                    rc = command_service->open(
                        command_service->user,
                        s_runtime,
                        &command_config,
                        "bk7258_v3_202405",
                        H2_LOADER_CAPABILITIES_LOADER,
                        &s_h2loader_ble_service);
                }
                if (rc != H2_PAL_OK) {
                    (void)snprintf(
                        line,
                        sizeof(line),
                        "H2_BK_H2LOADER_STEP stage=bleikcp_start rc=%d recovery=uart\r\n",
                        rc);
                    emergency_uart_write_string(0, line);
                } else {
                    emergency_uart_write_string(
                        0,
                        "H2_BK_H2LOADER_STEP stage=bleikcp_start rc=0\r\n");
                }
            }
            break;
        }
        rtos_delay_milliseconds(H2_BK_STARTUP_RETRY_DELAY_MS);
    }

    wait_forever();
}

static void h2loader_ap_entry(void *user) {
    h2_runtime_config_t runtime_config;

    (void)user;

    emergency_uart_write_string(0, "H2_BK_AP_ENTRY_EMERG image=h2loader\r\n");
    rtos_delay_milliseconds(500);
    emergency_uart_write_string(0, "H2_BK_AP_BOOT image=h2loader\r\n");

    emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=runtime_config begin\r\n");
    int rc = h2_bk7258_board_runtime_config(&runtime_config);
    emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=runtime_config end\r\n");
    if (rc == H2_PAL_OK) {
        emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=runtime_init begin\r\n");
        rc = h2_runtime_init(&runtime_config, &s_runtime);
        emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=runtime_init end\r\n");
    }
    if (rc != H2_PAL_OK) {
        record_startup_error("runtime_init", rc);
        wait_forever();
    }

    rc = h2_bk_h2loader_sd_fs_init(&s_h2loader_fs);
    if (rc != H2_PAL_OK) {
        record_startup_error("fs_init", rc);
        wait_forever();
    }
    emergency_uart_write_string(0, "H2_BK_H2LOADER_STEP stage=fs_init rc=0\r\n");

    h2loader_startup_worker(NULL);
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    emergency_uart_write_string(0, "H2_BK_AP_MAIN_EMERG stage=before_bk_init\r\n");
    os_printf("H2_BK_AP_MAIN stage=before_bk_init\r\n");
    bk_init();
    emergency_uart_write_string(0, "H2_BK_AP_MAIN_EMERG stage=after_bk_init\r\n");
    os_printf("H2_BK_AP_MAIN stage=after_bk_init\r\n");
    h2_pal_result_t rc = h2_bk7258_board_start_entry_task(
        "bk/h2loader", h2loader_ap_entry, NULL);
    if (rc != H2_PAL_OK) {
        char line[96];
        (void)snprintf(
            line,
            sizeof(line),
            "H2_BK_BOARD_ENTRY_FAIL image=h2loader rc=%d\r\n",
            rc);
        emergency_uart_write_string(0, line);
        os_printf("%s", line);
    }
    return 0;
}
