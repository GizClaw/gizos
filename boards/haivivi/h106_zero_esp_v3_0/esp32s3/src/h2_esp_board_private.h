#ifndef H2_ESP_BOARD_PRIVATE_H
#define H2_ESP_BOARD_PRIVATE_H

/* Serialized board bring-up only; attempt once per image, before consumers. */
void h2_esp_board_arena_init(void);

#include "h2_runtime.h"
#include "h2_esp_board_config.h"

/* Pure sizing policy shared by bring-up and host boundary tests. */
static inline size_t h2_esp_board_arena_reserved_bytes(size_t free_bytes,
                                                       size_t largest) {
  size_t reserved = free_bytes > H2_ZERO_ESP_ARENA_SYSTEM_RESERVE_BYTES
                        ? free_bytes - H2_ZERO_ESP_ARENA_SYSTEM_RESERVE_BYTES
                        : 0u;
  if (reserved > largest) reserved = largest;
  if (reserved > H2_ZERO_ESP_ARENA_BYTES) reserved = H2_ZERO_ESP_ARENA_BYTES;
  return reserved & ~(size_t)(64u * 1024u - 1u);
}

#include "os/h2_pal_fs.h"
#include "hal/h2_pal_audio.h"
#include "os/h2_pal_crypto.h"
#include "os/h2_pal_disk.h"
#include "hal/h2_pal_display.h"
#include "os/h2_pal_pref.h"
#include "hal/h2_pal_input.h"
#include "hal/h2_pal_led.h"
#include "os/h2_pal_log.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_sync.h"
#include "os/h2_pal_queue.h"
#include "os/h2_pal_task.h"
#include "os/h2_pal_time.h"
#include "os/h2_pal_system_event.h"
#include "hal/h2_pal_ble.h"
#include "application/h2_pal_webrtc.h"
#include "hal/h2_pal_wifi.h"
#include "hal/h2_pal_wifi_settings.h"
#include "hal/h2_pal_modem.h"
#include "hal/h2_pal_power.h"
#include "hal/h2_pal_pwm_switch.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_esp_board_fs_init(h2_pal_fs_api_t* fs);
int h2_esp_board_fs_deinit(void);
int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t* fs);
int h2_esp_board_h2loader_fs_deinit(void);
int h2_esp_board_fs_mount(const char* path);
int h2_esp_board_fs_unmount(const char* path);
int h2_esp_board_fs_clear(const char* path);
int h2_esp_board_fs_mount_all(void);
int h2_esp_board_fs_unmount_all(void);
int h2_esp_board_extflash_mount_data(void);
int h2_esp_board_extflash_unmount_data(void);
int h2_esp_board_extflash_clear_data(void);
h2_pal_mem_api_t* h2_esp_board_default_allocator(void);
h2_pal_mem_api_t* h2_esp_board_psram_allocator(void);
h2_pal_mem_api_t* h2_esp_board_internal_allocator(void);
h2_pal_mem_api_t* h2_esp_board_dma_allocator(void);
const h2_pal_log_api_t* h2_esp_board_log_api(void);
const h2_pal_sync_api_t* h2_esp_board_sync_api(void);
const h2_pal_task_api_t* h2_esp_board_task_api(void);
const h2_pal_queue_api_t* h2_esp_board_queue_api(void);
const h2_pal_time_api_t* h2_esp_board_time_api(void);
const h2_pal_system_event_api_t* h2_esp_board_system_event_api(void);
const h2_pal_crypto_api_t* h2_esp_board_crypto_api(void);
const h2_pal_power_api_t* h2_esp_board_power_api(void);
const h2_pal_disk_api_t* h2_esp_board_disk_api(void);
const h2_pal_pref_api_t* h2_esp_board_pref_api(void);
const h2_pal_webrtc_api_t* h2_esp_board_webrtc_api(void);
h2_pal_ble_t* h2_esp_board_ble(void);
h2_pal_wifi_sta_t* h2_esp_board_wifi_sta(void);
h2_pal_wifi_ap_t* h2_esp_board_wifi_ap(void);
h2_pal_wifi_settings_t* h2_esp_board_wifi_settings(void);
const h2_pal_periph_api_t* h2_esp_board_periph_api(void);
const h2_pal_led_api_t* h2_esp_board_led_api(void);
const h2_pal_button_api_t* h2_esp_board_button_api(void);
bool h2_esp_board_nfc_enabled(void);
const h2_pal_nfc_api_t* h2_esp_board_nfc_api(void);
h2_pal_input_api_t* h2_esp_board_input_api(void);
h2_pal_pwm_switch_api_t* h2_esp_board_pwm_switch_api(void);
h2_pal_modem_t* h2_esp_board_modem(void);
bool h2_esp_board_modem_is_powered(void);
/* Called only after a successful rail write, under its existing GPIO owner. */
void h2_esp_board_modem_record_power(bool powered);
h2_pal_result_t h2_esp_board_modem_prepare_ri_wake(void);
/* Owned sessions require current preparation; passive wakes preserve the rail.
 */
h2_pal_result_t h2_esp_board_modem_prepare_sleep(bool supervised);
/* Freezes the preparation receipt for a non-returning terminal commit. */
bool h2_esp_board_modem_sleep_commit(void);

h2_pal_display_t* h2_esp_board_display(void);
h2_pal_display_t* h2_esp_board_display_if_initialized(void);
h2_pal_audio_t* h2_esp_board_audio(void);
h2_pal_audio_t* h2_esp_board_audio_if_initialized(void);
h2_pal_result_t h2_esp_board_audio_deinit(void);
h2_pal_result_t h2_esp_board_audio_power_down(void);
h2_pal_result_t h2_esp_board_led_prepare_sleep(void);

#ifdef __cplusplus
}
#endif

#endif
