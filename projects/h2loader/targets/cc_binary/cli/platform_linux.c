#include "h2_h2loader_cli_target.h"

#include "h2_h2loader_cli_host_path.h"

#include <stdlib.h>
#include <unistd.h>

#include "h2_desktop_platform.h"
#include "h2_linux_platform.h"
#include "h2_linux_serial_host.h"

static h2_linux_host_fs_t *host_fs;

static const char *const fs_sources[] = {"/tmp", "/home"};
static const char *const fs_targets[] = {"/tmp", "/home"};

h2_pal_result_t h2_h2loader_cli_target_start(void) {
    h2_pal_result_t result = (h2_pal_result_t)h2_linux_host_fs_create(
        fs_sources, fs_targets, 2u, &host_fs);
    if (result != H2_PAL_OK) return result;
    result = (h2_pal_result_t)h2_pal_system_event_init(
        h2_linux_system_event_api());
    if (result != H2_PAL_OK) {
        h2_linux_host_fs_destroy(host_fs);
        host_fs = NULL;
    }
    return result;
}

void h2_h2loader_cli_target_stop(void) {
    h2_pal_system_event_deinit(h2_linux_system_event_api());
    h2_linux_host_fs_destroy(host_fs);
    host_fs = NULL;
}

const h2_pal_serial_host_api_t *h2_h2loader_cli_target_serial(void) { return h2_linux_serial_host_api(); }
const h2_pal_ble_host_api_t *h2_h2loader_cli_target_ble(const h2_pal_mem_api_t *mem) { (void)mem; return NULL; }
const h2_pal_system_event_api_t *h2_h2loader_cli_target_system_event(void) { return h2_linux_system_event_api(); }
const h2_pal_mem_api_t *h2_h2loader_cli_target_mem(void) { return h2_desktop_platform_default_allocator(); }
const h2_pal_time_api_t *h2_h2loader_cli_target_time(void) { return h2_desktop_platform_time_api(); }
const h2_pal_task_api_t *h2_h2loader_cli_target_task(void) { return h2_desktop_platform_task_api(); }
const h2_pal_sync_api_t *h2_h2loader_cli_target_sync(void) { return h2_desktop_platform_sync_api(); }
const h2_pal_queue_api_t *h2_h2loader_cli_target_queue(void) { return h2_desktop_platform_queue_api(); }
const h2_pal_log_api_t *h2_h2loader_cli_target_log(void) { return h2_desktop_platform_log_api(); }
const h2_pal_fs_api_t *h2_h2loader_cli_target_fs(void) { return h2_linux_host_fs_api(host_fs); }
const h2_pal_net_api_t *h2_h2loader_cli_target_net(void) { return h2_linux_net_api(); }

int h2_h2loader_cli_target_resolve_path(const char *path, char *out, size_t out_size) {
    char cwd[4096];
    const char *base = getenv("BUILD_WORKING_DIRECTORY");
    if (base == NULL || base[0] != '/') {
        base = getcwd(cwd, sizeof(cwd)) != NULL ? cwd : NULL;
    }
    return h2_h2loader_cli_host_path_resolve(
        base, fs_sources, fs_targets, 2u, path, out, out_size);
}
