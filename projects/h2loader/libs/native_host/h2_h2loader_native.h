#ifndef H2_H2LOADER_NATIVE_H
#define H2_H2LOADER_NATIVE_H

#include "h2_h2loader_host.h"
#include "h2_runtime.h"

#include <stddef.h>

h2_pal_result_t h2_h2loader_native_start(void);
void h2_h2loader_native_stop(void);
const h2_pal_serial_host_api_t* h2_h2loader_native_serial(void);
const h2_pal_ble_host_api_t* h2_h2loader_native_ble(
    const h2_pal_mem_api_t* mem);
const h2_pal_system_event_api_t* h2_h2loader_native_system_event(void);
const h2_pal_mem_api_t* h2_h2loader_native_mem(void);
const h2_pal_time_api_t* h2_h2loader_native_time(void);
const h2_pal_task_api_t* h2_h2loader_native_task(void);
const h2_pal_sync_api_t* h2_h2loader_native_sync(void);
const h2_pal_queue_api_t* h2_h2loader_native_queue(void);
const h2_pal_log_api_t* h2_h2loader_native_log(void);
const h2_pal_fs_api_t* h2_h2loader_native_fs(void);
const h2_pal_net_api_t* h2_h2loader_native_net(void);
/* Maps a user-supplied host path into the target's PAL fs mounts; returns 1 and
 * fills out when rewritten, 0 to pass the original through unchanged. */
int h2_h2loader_native_resolve_path(const char* path, char* out,
                                    size_t out_size);

#endif
