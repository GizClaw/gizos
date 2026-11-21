#include "h2_esp_board_private.h"
#include "h2_corehttp.h"
#include "esp_partition.h"

#include <assert.h>
#include <string.h>

static unsigned peripheral_inits, peripheral_closes, display_closes;
static unsigned http_creates, http_destroys, fs_mounts, fs_closes;
static int peripheral_close_error, display_close_error, fs_close_error;
static int http_create_error;
static int http_token;
static int fs_mkdir(void* user, const char* path) {
  assert(user == &http_token);
  assert(strcmp(path, "/data") == 0 || strcmp(path, "/dl") == 0);
  return H2_PAL_OK;
}
static const h2_pal_fs_vtable_t fs_vtable = {.mkdir = fs_mkdir};

int h2_passport_peripherals_init(void) {
  ++peripheral_inits;
  return 0;
}
int h2_passport_peripherals_deinit(void) {
  ++peripheral_closes;
  return peripheral_close_error;
}
int h2_passport_display_deinit(void) {
  ++display_closes;
  return display_close_error;
}
int h2_esp_platform_littlefs_fs_init(
    h2_pal_fs_api_t* fs, const h2_esp_platform_littlefs_config_t* config) {
  assert(strcmp(config->base_path, "/passport") == 0);
  assert(!config->format_if_mount_failed);
  ++fs_mounts;
  *fs = (h2_pal_fs_api_t){.user = &http_token, .vtable = &fs_vtable};
  return 0;
}
int h2_esp_platform_littlefs_fs_deinit(const char* label) {
  assert(strcmp(label, "data") == 0);
  ++fs_closes;
  return fs_close_error;
}
int h2_esp_platform_littlefs_format(const char* label) {
  (void)label;
  assert(0 && "mounted filesystem must not be formatted");
  return H2_PAL_ERR_IO;
}
const esp_partition_t* esp_partition_find_first(int type, int subtype,
                                                const char* label) {
  (void)type;
  (void)subtype;
  (void)label;
  assert(0 && "mounted filesystem needs no erase scan");
  return NULL;
}
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* data, size_t size) {
  (void)partition;
  (void)offset;
  (void)data;
  (void)size;
  assert(0);
  return -1;
}
h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* config,
                                   h2_corehttp_t** out_http,
                                   h2_pal_http_api_t* out_api) {
  (void)config;
  ++http_creates;
  *out_http = NULL;
  memset(out_api, 0, sizeof(*out_api));
  if (http_create_error) return http_create_error;
  *out_http = (h2_corehttp_t*)&http_token;
  out_api->user = &http_token;
  return 0;
}
void h2_corehttp_destroy(h2_corehttp_t* http) {
  if (http != NULL) {
    assert(http == (h2_corehttp_t*)&http_token);
    ++http_destroys;
  }
}

#define NULL_API(type, name) \
  type* name(void) { return NULL; }
NULL_API(h2_pal_mem_api_t, h2_esp_platform_internal_allocator)
NULL_API(const h2_pal_net_api_t, h2_esp_platform_net_api)
NULL_API(const h2_pal_netif_api_t, h2_esp_platform_netif_api)
NULL_API(const h2_pal_time_api_t, h2_esp_platform_time_api)
NULL_API(const h2_pal_log_api_t, h2_esp_platform_log_api)
NULL_API(const h2_pal_firmware_info_api_t, h2_esp_platform_firmware_info_api)
NULL_API(const h2_pal_timer_api_t, h2_esp_platform_timer_api)
NULL_API(const h2_pal_task_api_t, h2_esp_platform_task_api)
NULL_API(const h2_pal_queue_api_t, h2_esp_platform_queue_api)
NULL_API(const h2_pal_sync_api_t, h2_esp_platform_sync_api)
NULL_API(const h2_pal_disk_api_t, h2_esp_platform_disk_api)
NULL_API(const h2_pal_pref_api_t, h2_esp_platform_pref_api)
NULL_API(const h2_pal_crypto_api_t, h2_esp_platform_crypto_api)
NULL_API(const h2_pal_power_api_t, h2_esp_platform_power_api)
NULL_API(const h2_pal_system_event_api_t, h2_esp_platform_system_event_api)
NULL_API(h2_pal_wifi_sta_t, h2_esp_platform_wifi_sta)
NULL_API(h2_pal_wifi_ap_t, h2_esp_platform_wifi_ap)
NULL_API(h2_pal_wifi_settings_t, h2_esp_platform_wifi_settings)
NULL_API(h2_pal_ble_t, h2_esp_platform_ble)
NULL_API(const h2_pal_display_api_t, h2_passport_display)
NULL_API(h2_pal_audio_t, h2_passport_audio)
NULL_API(const h2_pal_periph_api_t, h2_passport_periph)
NULL_API(const h2_pal_button_api_t, h2_passport_button)
NULL_API(const h2_pal_input_api_t, h2_passport_input)
#undef NULL_API

static void assert_config_rejected(void) {
  h2_runtime_config_t config;
  memset(&config, 0xa5, sizeof(config));
  const unsigned inits = peripheral_inits, creates = http_creates;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE);
  const h2_runtime_config_t empty = {0};
  assert(memcmp(&config, &empty, sizeof(config)) == 0);
  assert(peripheral_inits == inits && http_creates == creates);
  assert(h2_esp_board_fs_mount("/data") == H2_PAL_ERR_INVALID_STATE);
}

static void test_failed_teardown(unsigned stage) {
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == 0);
  assert(config.http->user == &http_token);
  const unsigned creates = http_creates, destroys = http_destroys;
  h2_runtime_config_t cached;
  assert(h2_esp_board_runtime_config(&cached) == 0);
  assert(http_creates == creates);
  int* error = stage == 0   ? &peripheral_close_error
               : stage == 1 ? &display_close_error
                            : &fs_close_error;
  *error = H2_PAL_ERR_IO;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO);
  assert_config_rejected();
  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO);
  assert_config_rejected();
  if (stage == 2) assert(http_destroys == destroys + 1u);
  *error = 0;
  assert(h2_esp_board_runtime_deinit() == 0);
  assert(http_destroys == destroys + 1u);
  assert(h2_esp_board_runtime_config(&config) == 0);
  assert(http_creates == creates + 1u);
  assert(config.http->user == &http_token);
  assert(h2_esp_board_runtime_deinit() == 0);
}

int main(void) {
  for (unsigned stage = 0; stage < 3; ++stage) test_failed_teardown(stage);
  http_create_error = H2_PAL_ERR_NO_MEMORY;
  fs_close_error = H2_PAL_ERR_IO;
  h2_runtime_config_t config;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_NO_MEMORY);
  assert_config_rejected();
  fs_close_error = 0;
  assert(h2_esp_board_runtime_deinit() == 0);
  http_create_error = 0;
  assert(h2_esp_board_runtime_config(&config) == 0);
  assert(h2_esp_board_runtime_deinit() == 0);
  assert(fs_mounts > 0u && fs_closes >= fs_mounts);
  assert(peripheral_closes >= peripheral_inits && display_closes > 0u);
  return 0;
}
