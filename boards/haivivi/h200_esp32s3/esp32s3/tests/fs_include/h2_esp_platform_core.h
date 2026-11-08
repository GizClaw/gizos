#ifndef H2_ESP_PLATFORM_CORE_TEST_SHADOW_H
#define H2_ESP_PLATFORM_CORE_TEST_SHADOW_H

/* Host-test shadow of
 * native_component_src/esp-idf6.x/h2_pal_core/include/h2_esp_platform_core.h.
 * It declares only the accessors h2_esp_board_runtime.c consumes. */

#include "hal/h2_pal_wifi.h"
#include "os/h2_pal_firmware_info.h"
#include "os/h2_pal_timer.h"
#include "net/h2_pal_net.h"
#include "net/h2_pal_netif.h"

struct h2_esp_task_policy;
typedef struct h2_esp_task_policy_config {
  h2_pal_result_t (*resolver)(void*, const char*, struct h2_esp_task_policy*);
  void* resolver_user;
  const h2_pal_mem_api_t* psram_stack_allocator;
} h2_esp_task_policy_config_t;
h2_pal_result_t h2_esp_platform_task_configure(
    const h2_esp_task_policy_config_t* config);

const h2_pal_device_identity_api_t* h2_esp_platform_device_identity_api(void);
const h2_pal_io_stream_api_t* h2_esp_platform_io_stream_api(void);
const h2_pal_firmware_info_api_t* h2_esp_platform_firmware_info_api(void);
const h2_pal_timer_api_t* h2_esp_platform_timer_api(void);
const h2_pal_net_api_t* h2_esp_platform_net_api(void);
const h2_pal_netif_api_t* h2_esp_platform_netif_api(void);
const h2_pal_wifi_csi_api_t* h2_esp_platform_wifi_csi(void);

typedef struct {
  const char* base_path;
  const char* partition_label;
  bool format_if_mount_failed;
} h2_esp_platform_littlefs_config_t;
typedef struct {
  const char* base_path;
  const char* partition_label;
  size_t max_files;
  bool format_if_mount_failed;
} h2_esp_platform_spiffs_config_t;
int h2_esp_platform_data_partition_subtype(const char*, uint8_t*);
int h2_esp_platform_littlefs_fs_init(h2_pal_fs_api_t*,
                                     const h2_esp_platform_littlefs_config_t*);
int h2_esp_platform_littlefs_fs_deinit(const char*);
int h2_esp_platform_spiffs_fs_init(h2_pal_fs_api_t*,
                                   const h2_esp_platform_spiffs_config_t*);
int h2_esp_platform_spiffs_fs_deinit(const char*);
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t*, const char*);
int h2_esp_platform_littlefs_mount(const h2_esp_platform_littlefs_config_t*);
int h2_esp_platform_littlefs_format(const char*);
#endif
