#include "h2_h2loader_native.h"

h2_pal_result_t h2_h2loader_native_start(void) { return H2_PAL_ERR_IO; }
void h2_h2loader_native_stop(void) {}

#define EMPTY_ACCESSOR(name, type) \
  const type* name(void) { return NULL; }

EMPTY_ACCESSOR(h2_h2loader_native_serial, h2_pal_serial_host_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_system_event, h2_pal_system_event_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_mem, h2_pal_mem_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_time, h2_pal_time_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_task, h2_pal_task_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_sync, h2_pal_sync_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_queue, h2_pal_queue_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_log, h2_pal_log_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_fs, h2_pal_fs_api_t)
EMPTY_ACCESSOR(h2_h2loader_native_net, h2_pal_net_api_t)

const h2_pal_ble_host_api_t* h2_h2loader_native_ble(
    const h2_pal_mem_api_t* mem) {
  (void)mem;
  return NULL;
}

int h2_h2loader_native_resolve_path(const char* path, char* out,
                                    size_t out_size) {
  (void)path;
  (void)out;
  (void)out_size;
  return 0;
}
