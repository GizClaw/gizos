#ifndef H2_H2LOADER_CLI_TARGET_H
#define H2_H2LOADER_CLI_TARGET_H

#include "h2_h2loader_cli_app.h"

#include <stddef.h>

h2_pal_result_t h2_h2loader_cli_target_start(void);
void h2_h2loader_cli_target_stop(void);
const h2_pal_serial_host_api_t *h2_h2loader_cli_target_serial(void);
const h2_pal_ble_host_api_t *h2_h2loader_cli_target_ble(const h2_pal_mem_api_t *mem);
const h2_pal_system_event_api_t *h2_h2loader_cli_target_system_event(void);
const h2_pal_mem_api_t *h2_h2loader_cli_target_mem(void);
const h2_pal_time_api_t *h2_h2loader_cli_target_time(void);
const h2_pal_task_api_t *h2_h2loader_cli_target_task(void);
const h2_pal_sync_api_t *h2_h2loader_cli_target_sync(void);
const h2_pal_queue_api_t *h2_h2loader_cli_target_queue(void);
const h2_pal_log_api_t *h2_h2loader_cli_target_log(void);
const h2_pal_fs_api_t *h2_h2loader_cli_target_fs(void);
const h2_pal_net_api_t *h2_h2loader_cli_target_net(void);
/* Maps a user-supplied host path into the target's PAL fs mounts; returns 1 and
 * fills out when rewritten, 0 to pass the original through unchanged. */
int h2_h2loader_cli_target_resolve_path(const char *path, char *out, size_t out_size);

#endif
