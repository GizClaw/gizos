#include "h2_h2loader_cli_target.h"

h2_pal_result_t h2_h2loader_cli_target_start(void) { return H2_PAL_ERR_IO; }
void h2_h2loader_cli_target_stop(void) {}

#define EMPTY_ACCESSOR(name, type) \
    const type *name(void) { return NULL; }

EMPTY_ACCESSOR(h2_h2loader_cli_target_serial, h2_pal_serial_host_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_system_event, h2_pal_system_event_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_mem, h2_pal_mem_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_time, h2_pal_time_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_task, h2_pal_task_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_sync, h2_pal_sync_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_queue, h2_pal_queue_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_log, h2_pal_log_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_fs, h2_pal_fs_api_t)
EMPTY_ACCESSOR(h2_h2loader_cli_target_net, h2_pal_net_api_t)

const h2_pal_ble_host_api_t *h2_h2loader_cli_target_ble(
    const h2_pal_mem_api_t *mem) {
    (void)mem;
    return NULL;
}

int h2_h2loader_cli_target_resolve_path(
    const char *path, char *out, size_t out_size) {
    (void)path;
    (void)out;
    (void)out_size;
    return 0;
}
