#include "h2_bk7258_board.h"
#include "h2_bk7258_board_storage.h"
#include "h2_bk7258_board_private.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_platform_image.h"
#include "h2_corehttp.h"
#include "h2_bk7258_board_runtime_test_sdk.h"

/* Reuse the native provider's deterministic SD/FatFs fake, including its
 * work-area identity and power-cycle assertions. Compile both real providers.
 */
#define main native_sd_fs_fixture_main
#include "native_component_src/bk7258/ap/h2_pal_storage/tests/test_h2_bk_platform_sd_fs.c"
#undef main

static unsigned disk_configs, boot_starts, business_calls, http_creates,
    http_destroys;
static int disk_error, boot_error, http_error, http_instance;

FRESULT f_lseek(FIL* file, FSIZE_t offset) {
  file->marker = (int)offset;
  return FR_OK;
}
int h2_bk_platform_disk_configure(const h2_bk_disk_region_t* region) {
  assert(region->id == H2_BK7258_COREDUMP_ADDR);
  ++disk_configs;
  return disk_error;
}
int h2_bk_platform_boot_startup(void) {
  ++boot_starts;
  assert(disk_configs > 0u && attempts == 0u);
  return boot_error;
}

#define NATIVE_API(type, name, capability) \
  const type* name(void) { return h2_pal_unsupported_##capability##_api(); }
NATIVE_API(h2_pal_firmware_info_api_t, h2_bk_platform_firmware_info_api,
           firmware_info)
NATIVE_API(h2_pal_device_identity_api_t, h2_bk_platform_device_identity_api,
           device_identity)
NATIVE_API(h2_pal_image_api_t, h2_bk_platform_image_api, image)
NATIVE_API(h2_pal_io_stream_api_t, h2_bk_platform_io_stream_api, io_stream)
NATIVE_API(h2_pal_log_api_t, h2_bk_platform_log_api, log)
NATIVE_API(h2_pal_time_api_t, h2_bk_platform_time_api, time)
NATIVE_API(h2_pal_task_api_t, h2_bk_platform_task_api, task)
NATIVE_API(h2_pal_queue_api_t, h2_bk_platform_queue_api, queue)
NATIVE_API(h2_pal_sync_api_t, h2_bk_platform_sync_api, sync)
NATIVE_API(h2_pal_pref_api_t, h2_bk_platform_pref_api, pref)
NATIVE_API(h2_pal_crypto_api_t, h2_bk_platform_crypto_api, crypto)
NATIVE_API(h2_pal_system_event_api_t, h2_bk_platform_system_event_api,
           system_event)
NATIVE_API(h2_pal_power_api_t, h2_bk_platform_power_api, power)
NATIVE_API(h2_pal_disk_api_t, h2_bk_platform_disk_api, disk)
#undef NATIVE_API

h2_pal_mem_api_t* h2_bk_platform_default_allocator(void) {
  static h2_pal_mem_api_t api;
  api = *h2_pal_unsupported_mem_api();
  return &api;
}
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  static h2_pal_mem_api_t api;
  api = *h2_pal_unsupported_mem_api();
  return &api;
}
h2_pal_ble_t* h2_bk_platform_ble(void) {
  return (h2_pal_ble_t*)h2_pal_unsupported_ble_host_api();
}

/* Full-profile dependencies must stay untouched by recovery configuration. */
#define BUSINESS_API(type, name, capability)               \
  type* name(void) {                                       \
    ++business_calls;                                      \
    return (type*)h2_pal_unsupported_##capability##_api(); \
  }
BUSINESS_API(h2_pal_mem_api_t, h2_bk7258_board_default_allocator, mem)
BUSINESS_API(h2_pal_mem_api_t, h2_bk7258_board_psram_allocator, mem)
BUSINESS_API(const h2_pal_log_api_t, h2_bk7258_board_log_api, log)
BUSINESS_API(const h2_pal_time_api_t, h2_bk7258_board_time_api, time)
BUSINESS_API(const h2_pal_task_api_t, h2_bk7258_board_task_api, task)
BUSINESS_API(const h2_pal_sync_api_t, h2_bk7258_board_sync_api, sync)
BUSINESS_API(const h2_pal_queue_api_t, h2_bk7258_board_queue_api, queue)
BUSINESS_API(const h2_pal_pref_api_t, h2_bk7258_board_pref_api, pref)
BUSINESS_API(const h2_pal_webrtc_api_t, h2_bk7258_board_webrtc_api, webrtc)
BUSINESS_API(h2_pal_wifi_sta_t, h2_bk7258_board_wifi_sta, wifi_sta)
BUSINESS_API(h2_pal_wifi_ap_t, h2_bk7258_board_wifi_ap, wifi_ap)
BUSINESS_API(h2_pal_wifi_settings_t, h2_bk7258_board_wifi_settings,
             wifi_settings)
BUSINESS_API(h2_pal_ble_t, h2_bk7258_board_ble, ble_host)
BUSINESS_API(h2_pal_modem_t, h2_bk7258_board_modem, modem)
BUSINESS_API(h2_pal_display_t, h2_bk7258_board_display, display)
BUSINESS_API(const h2_pal_periph_api_t, h2_bk7258_board_periph_api, periph)
BUSINESS_API(const h2_pal_button_api_t, h2_bk7258_board_button_api, button)
BUSINESS_API(const h2_pal_touch_api_t, h2_bk7258_board_touch_api, touch)
BUSINESS_API(h2_pal_input_api_t, h2_bk7258_board_input_api, input)
BUSINESS_API(const h2_pal_system_event_api_t, h2_bk7258_board_system_event_api,
             system_event)
BUSINESS_API(const h2_pal_timer_api_t, h2_bk_platform_timer_api, timer)
BUSINESS_API(const h2_pal_net_api_t, h2_bk_platform_net_api, net)
BUSINESS_API(const h2_pal_netif_api_t, h2_bk_platform_netif_api, netif)
BUSINESS_API(const h2_pal_mqtt_api_t, h2_bk_platform_mqtt_api, mqtt)
BUSINESS_API(const h2_pal_wifi_csi_api_t, h2_bk_platform_wifi_csi, wifi_csi)
#undef BUSINESS_API

h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* config,
                                   h2_corehttp_t** http,
                                   h2_pal_http_api_t* api) {
  assert(config != NULL && http != NULL && api != NULL);
  ++business_calls;
  ++http_creates;
  *http = NULL;
  if (http_error) return http_error;
  *http = (h2_corehttp_t*)&http_instance;
  *api = *h2_pal_unsupported_http_api();
  return H2_PAL_OK;
}
void h2_corehttp_destroy(h2_corehttp_t* http) {
  assert(http == (h2_corehttp_t*)&http_instance);
  ++http_destroys;
}

int main(void) {
  h2_runtime_config_t config;
  assert(h2_bk7258_board_recovery_runtime_config(NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  disk_error = H2_PAL_ERR_IO;
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_ERR_IO);
  assert(disk_configs == 1u && boot_starts == 0u && config.board == NULL);
  disk_error = 0;
  boot_error = H2_PAL_ERR_IO;
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_ERR_IO);
  assert(disk_configs == 2u && boot_starts == 1u && config.board == NULL);
  boot_error = 0;
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_OK);
  assert(!strcmp(config.board, "bk7258_v3_202405"));
  assert(config.io_stream == h2_bk_platform_io_stream_api());
  assert(config.mem == h2_bk_platform_psram_allocator());
  assert(config.image == h2_bk_platform_image_api());
  assert(config.device_identity == h2_bk_platform_device_identity_api());
  assert(config.pref == h2_bk_platform_pref_api());
  assert(config.http == h2_pal_unsupported_http_api());
  assert(config.net == h2_pal_unsupported_net_api());
  assert(config.display == h2_pal_unsupported_display_api());
  assert(config.audio == h2_pal_unsupported_audio_api());
  assert(config.timer == h2_pal_unsupported_timer_api());
  assert(config.system_event == h2_bk_platform_system_event_api());
  assert(disk_configs == 3u && boot_starts == 2u && !business_calls &&
         !attempts);
  const h2_pal_fs_api_t* recovery_fs = config.fs;
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_OK);
  assert(config.fs == recovery_fs && disk_configs == 3u && boot_starts == 2u);
  failures = 4u;
  assert(h2_pal_fs_prepare(recovery_fs, "/dl") == H2_PAL_OK);
  assert(attempts == 5u && power_off_waits == 5u && power_on_waits == 5u);
  assert(h2_pal_fs_prepare(recovery_fs, "/data") == H2_PAL_OK &&
         attempts == 5u);
  h2_pal_fs_file_t* file;
  assert(h2_pal_fs_open(recovery_fs, "/dl/file", H2_PAL_FS_OPEN_READ, &file) ==
         H2_PAL_OK);
  assert(!strcmp(last_path, "1:/h2loader/dl/file"));
  assert(h2_pal_fs_close(recovery_fs, file) == H2_PAL_OK);
  assert(h2_pal_fs_clear(recovery_fs, "/data") == H2_PAL_OK);
  assert(!strcmp(last_path, "1:/h2loader/data") && !business_calls);
  unsigned saved_unmounts = unmounts;
  h2_runtime_config_t full;
  http_error = H2_PAL_ERR_NO_MEMORY;
  assert(h2_bk7258_board_runtime_config(&full) == H2_PAL_ERR_NO_MEMORY);
  assert(full.board == NULL && attempts == 5u && unmounts == saved_unmounts);
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_OK);
  assert(config.fs == recovery_fs);
  http_error = 0;
  assert(h2_bk7258_board_runtime_config(&full) == H2_PAL_OK);
  assert(full.fs->vtable == recovery_fs->vtable);
  assert(full.fs->user == recovery_fs->user);
  assert(attempts == 5u && unmounts == saved_unmounts && boot_starts == 2u);
  assert(full.io_stream == config.io_stream && full.power == config.power);
  /* The existing manufacturing extension preserves the original FS/Power
   * pointers after selecting the board's complete provider surface. */
  full.fs = config.fs;
  full.power = config.power;
  config = full;
  assert(h2_pal_fs_prepare(config.fs, "/data") == H2_PAL_OK && attempts == 5u);
  assert(h2_pal_fs_clear(config.fs, "/data") == H2_PAL_OK);
  unsigned saved_business = business_calls;
  assert(h2_bk7258_board_runtime_config(&full) == H2_PAL_OK);
  assert(business_calls == saved_business && http_creates == 2u);
  assert(h2_bk7258_board_runtime_deinit() == H2_PAL_OK);
  assert(unmounts == saved_unmounts + 1u && http_destroys == 1u);
  assert(h2_bk7258_board_runtime_deinit() == H2_PAL_OK);
  assert(unmounts == saved_unmounts + 1u && http_destroys == 1u);
  assert(h2_bk7258_board_recovery_runtime_config(&config) == H2_PAL_OK);
  assert(boot_starts == 2u && config.fs == recovery_fs &&
         business_calls == saved_business);
  failures = 5u;
  attempts = 0u;
  assert(h2_pal_fs_prepare(config.fs, "/dl") == H2_PAL_ERR_IO &&
         attempts == 5u);
  failures = 0u;
  assert(h2_pal_fs_prepare(config.fs, "/dl") == H2_PAL_OK && attempts == 6u);
  assert(h2_bk7258_board_runtime_deinit() == H2_PAL_OK);
  assert(business_calls == saved_business && http_destroys == 1u);
  return 0;
}
