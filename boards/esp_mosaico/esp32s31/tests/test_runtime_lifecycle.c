#include "h2_mosaico_camera.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bsp/esp_mosaico.h"
#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_chip_board.h"
#include "h2_mosaico_audio.h"
#include "h2_mosaico_display.h"
#include "h2_mosaico_peripherals.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_libco_stack_guard.h"
#include "h2_corehttp.h"
#include <assert.h>
#include <string.h>
static int audio_error, fs_error, fs_init_error, fs_retained, create_error;
static unsigned creates, destroys, fs_stops;
static int provider;
/* Native stack switching is covered by the platform adapter tests. */
void h2_esp_libco_stack_guard_link(void) {}
int bsp_board_variant_get(bsp_board_variant_t* out) {
  *out = BSP_BOARD_VARIANT_V1_2;
  return 0;
}
int bsp_power_init(void) { return 0; }
int bsp_i2c_init(void) { return 0; }
int h2_mosaico_peripherals_init(void) { return 0; }
int h2_mosaico_peripherals_deinit(void) { return 0; }
int h2_mosaico_display_init(void) { return 0; }
int h2_mosaico_display_deinit(void) { return 0; }
int h2_mosaico_audio_init(void) { return 0; }
int h2_mosaico_audio_deinit(void) { return audio_error; }
int h2_esp_board_fs_init(h2_pal_fs_api_t* fs) {
  memset(fs, 0, sizeof(*fs));
  fs_retained = 1; /* /dl acquired, even if /data and rollback fail. */
  return fs_init_error;
}
int h2_esp_board_fs_deinit(void) {
  ++fs_stops;
  if (!fs_error) fs_retained = 0;
  return fs_error;
}
int h2_esp_board_fs_clear(const char* path) {
  (void)path;
  return 0;
}
h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* cfg,
                                   h2_corehttp_t** out,
                                   h2_pal_http_api_t* api) {
  assert(cfg);
  if (create_error) return create_error;
  ++creates;
  *out = (h2_corehttp_t*)&provider;
  memset(api, 0, sizeof(*api));
  return 0;
}
void h2_corehttp_destroy(h2_corehttp_t* http) {
  assert(http == (h2_corehttp_t*)&provider);
  ++destroys;
}
int main(void) {
  h2_runtime_config_t config;
  fs_init_error = fs_error = H2_PAL_ERR_IO;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_IO);
  assert(fs_retained && fs_stops == 1 && creates == 0 && config.board == NULL);
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO && fs_retained);
  fs_error = 0;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_OK && !fs_retained);
  fs_init_error = 0;
  fs_stops = 0;
  assert(h2_esp_board_runtime_config(&config) == 0 && creates == 1);
  assert(config.camera == h2_mosaico_camera());
  assert(h2_esp_board_runtime_config(&config) == 0 && creates == 1);
  assert(config.camera == h2_mosaico_camera());
  audio_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_TIMEOUT && destroys == 0);
  memset(&config, 0xff, sizeof(config));
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE &&
         config.board == NULL && creates == 1);
  audio_error = 0;
  fs_error = H2_PAL_ERR_IO;
  assert(h2_esp_board_runtime_deinit() == H2_PAL_ERR_IO && destroys == 0);
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE);
  fs_error = 0;
  assert(h2_esp_board_runtime_deinit() == 0 && destroys == 1 && fs_stops == 2);
  assert(h2_esp_board_runtime_deinit() == 0 && destroys == 1 && fs_stops == 2);
  create_error = H2_PAL_ERR_NO_MEMORY;
  fs_error = H2_PAL_ERR_IO;
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_IO);
  assert(h2_esp_board_runtime_config(&config) == H2_PAL_ERR_INVALID_STATE);
  fs_error = 0;
  assert(h2_esp_board_runtime_deinit() == 0);
  create_error = 0;
  assert(h2_esp_board_runtime_config(&config) == 0 && creates == 2);
  assert(h2_esp_board_runtime_deinit() == 0 && destroys == 2);
  return 0;
}

h2_pal_mem_api_t* h2_esp_board_default_allocator(void) { return NULL; }

h2_pal_mem_api_t* h2_esp_board_psram_allocator(void) { return NULL; }

const h2_pal_log_api_t* h2_esp_board_log_api(void) { return NULL; }

const h2_pal_sync_api_t* h2_esp_board_sync_api(void) { return NULL; }

const h2_pal_task_api_t* h2_esp_board_task_api(void) { return NULL; }

const h2_pal_queue_api_t* h2_esp_board_queue_api(void) { return NULL; }

const h2_pal_time_api_t* h2_esp_board_time_api(void) { return NULL; }

const h2_pal_system_event_api_t* h2_esp_board_system_event_api(void) {
  return NULL;
}

const h2_pal_crypto_api_t* h2_esp_board_crypto_api(void) { return NULL; }

const h2_pal_power_api_t* h2_esp_board_power_api(void) { return NULL; }

const h2_pal_disk_api_t* h2_esp_board_disk_api(void) { return NULL; }

const h2_pal_pref_api_t* h2_esp_board_pref_api(void) { return NULL; }

const h2_pal_webrtc_api_t* h2_esp_board_webrtc_api(void) { return NULL; }

h2_pal_ble_t* h2_esp_board_ble(void) { return NULL; }

h2_pal_wifi_sta_t* h2_esp_board_wifi_sta(void) { return NULL; }

h2_pal_wifi_ap_t* h2_esp_board_wifi_ap(void) { return NULL; }

h2_pal_wifi_settings_t* h2_esp_board_wifi_settings(void) { return NULL; }

h2_pal_modem_t* h2_esp_board_modem(void) { return NULL; }

const h2_pal_audio_api_t* h2_mosaico_audio(void) { return NULL; }

const h2_pal_display_api_t* h2_mosaico_display(void) { return NULL; }

const h2_pal_touch_api_t* h2_mosaico_touch(void) { return NULL; }

const h2_pal_periph_api_t* h2_mosaico_periph(void) { return NULL; }

const h2_pal_button_api_t* h2_mosaico_button(void) { return NULL; }

const h2_pal_imu_api_t* h2_mosaico_imu(void) { return NULL; }

const h2_pal_input_api_t* h2_mosaico_input(void) { return NULL; }

const h2_pal_pwm_switch_api_t* h2_mosaico_motor(void) { return NULL; }

const h2_pal_timer_api_t* h2_esp_platform_timer_api(void) { return NULL; }

const h2_pal_firmware_info_api_t* h2_esp_platform_firmware_info_api(void) {
  return NULL;
}

const h2_pal_net_api_t* h2_esp_platform_net_api(void) { return NULL; }

const h2_pal_netif_api_t* h2_esp_platform_netif_api(void) { return NULL; }

const h2_pal_wifi_csi_api_t* h2_esp_platform_wifi_csi(void) { return NULL; }

const h2_pal_camera_api_t* h2_mosaico_camera(void) {
  return h2_pal_unsupported_camera_api();
}
