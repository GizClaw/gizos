#include "h2_h2loader_cli_target.h"

#include "h2_windows_platform.h"
#include "h2_windows_serial_host.h"

static h2_windows_platform_t *platform;

h2_pal_result_t h2_h2loader_cli_target_start(void) {
    return h2_windows_platform_create_with_logical_drives(&platform);
}

void h2_h2loader_cli_target_stop(void) {
    (void)h2_windows_platform_destroy(&platform);
}

const h2_pal_serial_host_api_t *h2_h2loader_cli_target_serial(void) {
    return h2_windows_serial_host_api();
}

const h2_pal_ble_host_api_t *h2_h2loader_cli_target_ble(
    const h2_pal_mem_api_t *mem) {
    (void)mem;
    return NULL;
}

const h2_pal_system_event_api_t *h2_h2loader_cli_target_system_event(void) {
    return h2_windows_system_event_api(platform);
}

const h2_pal_mem_api_t *h2_h2loader_cli_target_mem(void) {
    return h2_windows_mem_api(platform);
}

const h2_pal_time_api_t *h2_h2loader_cli_target_time(void) {
    return h2_windows_time_api(platform);
}

const h2_pal_task_api_t *h2_h2loader_cli_target_task(void) {
    return h2_windows_task_api(platform);
}

const h2_pal_sync_api_t *h2_h2loader_cli_target_sync(void) {
    return h2_windows_sync_api(platform);
}

const h2_pal_queue_api_t *h2_h2loader_cli_target_queue(void) {
    return h2_windows_queue_api(platform);
}

const h2_pal_log_api_t *h2_h2loader_cli_target_log(void) {
    return h2_windows_log_api(platform);
}

const h2_pal_fs_api_t *h2_h2loader_cli_target_fs(void) {
    return h2_windows_fs_api(platform);
}

const h2_pal_net_api_t *h2_h2loader_cli_target_net(void) {
    return h2_windows_net_api(platform);
}

int h2_h2loader_cli_target_resolve_path(const char *path, char *out, size_t out_size) {
    (void)path;
    (void)out;
    (void)out_size;
    return 0;
}
