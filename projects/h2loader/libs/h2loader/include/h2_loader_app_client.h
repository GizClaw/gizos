#ifndef H2_LOADER_APP_CLIENT_H
#define H2_LOADER_APP_CLIENT_H

#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2_loader_memory.h"
#include "h2_loader_command.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Special read_byte result that discards an incomplete command line without
 * stopping the return-console task. It is distinct from EOF and byte values.
 */
#define H2_LOADER_APP_CLIENT_SESSION_RESET (-2)
/** Permanent transport closure; the console task exits without replay. */
#define H2_LOADER_APP_CLIENT_SESSION_CLOSED (-3)

typedef struct h2_loader_app_client_config {
    const h2_pal_pref_api_t *pref;
    const h2_pal_power_api_t *power;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_disk_api_t *disk;
    const h2_pal_fs_api_t *fs;
    const h2_pal_http_api_t *http;
    const h2_pal_wifi_sta_api_t *wifi;
    const h2_pal_wifi_settings_api_t *wifi_settings;
    h2_loader_digest_api_t digest;
    /** Optional shared owner lock for serial/BLE command execution. */
    const h2_pal_sync_api_t *operation_sync;
    h2_pal_mutex_t *operation_mutex;
    const h2_pal_sync_api_t *wifi_operation_sync;
    h2_pal_mutex_t *wifi_operation_mutex;
    const char *board;
    const char *target;
    const char *chip;
    /** Device-reported BLE public/identity MAC as 12 lowercase hex digits. */
    const char *device_uid;
    h2_loader_image_identity_t active_identity;
    /** Stable hardware facilities wired by the complete running App image. */
    uint32_t hardware_capabilities;
    uint32_t h2loader_partition_id;
    uint32_t app_partition_id;
    uint32_t coredump_partition_id;
    uint32_t reboot_reason;
    void *clock_user;
    uint64_t (*now_ms)(void *user);
    void (*sleep_ms)(void *user, uint32_t delay_ms);
    /** Optional platform memory snapshot exposed by `h2loader memory`. */
    h2_loader_memory_stats_api_t memory_stats;
} h2_loader_app_client_config_t;

typedef struct h2_loader_app_client {
    h2_loader_app_client_config_t config;
    h2_loader_t loader;
    const h2_pal_task_api_t *return_console_task_api;
    h2_pal_task_t *return_console_task;
    void *return_console_private;
} h2_loader_app_client_t;

typedef int (*h2_loader_app_client_write_fn)(
    void *user,
    const char *data,
    size_t len);

typedef struct h2_loader_app_client_return_console_config {
    h2_loader_app_client_t *client;
    const h2_pal_task_api_t *task;
    void *read_user;
    /** Returns one byte, EOF when no byte is available, or SESSION_RESET. */
    int (*read_byte)(void *user, uint32_t timeout_ms);
    void *write_user;
    h2_loader_app_client_write_fn write;
    const char *task_name;
    size_t stack_size;
} h2_loader_app_client_return_console_config_t;

int h2_loader_app_client_init(
    h2_loader_app_client_t *client,
    const h2_loader_app_client_config_t *config);
/**
 * Executes an App coredump subcommand over a caller-owned output stream.
 * The caller owns command serialization; a null subcommand selects status.
 */
int h2_loader_app_client_coredump(
    h2_loader_app_client_t *client,
    const char *subcommand,
    void *write_user,
    h2_loader_app_client_write_fn write);
int h2_loader_app_client_start_return_console(
    const h2_loader_app_client_return_console_config_t *config);
/** Joins a console that exited because its transport closed. */
int h2_loader_app_client_join_return_console(h2_loader_app_client_t *client);
int h2_loader_app_client_stop_return_console(h2_loader_app_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
