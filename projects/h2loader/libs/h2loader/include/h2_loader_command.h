#ifndef H2_LOADER_COMMAND_H
#define H2_LOADER_COMMAND_H

#include "h2_command.h"
#include "h2_loader_boot.h"
#include "h2_loader_memory.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_loader_command_config {
    h2_loader_t *loader;
    const h2_pal_fs_api_t *fs;
    const h2_pal_http_api_t *http;
    const h2_pal_wifi_sta_api_t *wifi;
    const h2_pal_wifi_settings_api_t *wifi_settings;
    const h2_pal_disk_api_t *disk;
    h2_loader_digest_api_t digest;
    h2_loader_memory_stats_api_t memory_stats;
    void *clock_user;
    uint64_t (*now_ms)(void *user);
    void (*sleep_ms)(void *user, uint32_t delay_ms);
    h2_command_io_api_t io;
    uint32_t coredump_partition_id;
    const h2_pal_sync_api_t *operation_sync;
    h2_pal_mutex_t *operation_mutex;
    const h2_pal_sync_api_t *wifi_operation_sync;
    h2_pal_mutex_t *wifi_operation_mutex;
    /** Leaves requested App installation to the Loader lifecycle task. */
    int defer_app_install;
} h2_loader_command_config_t;

#define H2_LOADER_COMMAND_DEFINITION_CAPACITY 9u
#define H2_LOADER_COMMAND_ROUTE_NODE_CAPACITY 64u
#define H2_LOADER_COMMAND_INPUT_BUFFER_SIZE 1024u
#define H2_LOADER_COMMAND_ARGV_CAPACITY 8u

typedef struct h2_loader_command {
    h2_command_t command;
    h2_loader_command_config_t config;
    h2_command_definition_t definitions[H2_LOADER_COMMAND_DEFINITION_CAPACITY];
    h2_trie_route_t routes[H2_LOADER_COMMAND_DEFINITION_CAPACITY];
    h2_trie_node_t route_nodes[H2_LOADER_COMMAND_ROUTE_NODE_CAPACITY];
    char input_buffer[H2_LOADER_COMMAND_INPUT_BUFFER_SIZE];
    const char *argv[H2_LOADER_COMMAND_ARGV_CAPACITY];
} h2_loader_command_t;

int h2_loader_command_init(
    h2_loader_command_t *command,
    const h2_loader_command_config_t *config);

/** @brief Execute one already-tokenized H2Loader command synchronously. */
int h2_loader_command_execute(
    h2_loader_command_t *command,
    size_t argc,
    const char *const *argv);
int h2_loader_command_poll(h2_loader_command_t *command, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
