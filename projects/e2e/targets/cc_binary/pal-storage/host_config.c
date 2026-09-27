#include "host_config.h"
#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"
h2_runtime_config_t h2_storage_host_config(const h2_pal_mem_api_t *memory) {
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "pal-storage", "desktop", "host", memory, h2_desktop_platform_time_api(),
      h2_desktop_platform_queue_api(), h2_pal_unsupported_display_api());
  config.task = h2_desktop_platform_task_api();
  config.sync = h2_desktop_platform_sync_api();
  return config;
}
