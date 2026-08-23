#ifndef H2_ESP_BOARD_PRIVATE_H
#define H2_ESP_BOARD_PRIVATE_H

#include "h2_runtime.h"

#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/hal/h2_pal_input.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/hal/h2_pal_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_esp_board_fs_init(h2_pal_fs_api_t *fs);
int h2_esp_board_fs_deinit(void);
int h2_esp_board_fs_mount_all(void);
int h2_esp_board_fs_unmount_all(void);
h2_pal_mem_api_t *h2_esp_board_default_allocator(void);
h2_pal_mem_api_t *h2_esp_board_psram_allocator(void);
h2_pal_mem_api_t *h2_esp_board_internal_allocator(void);
h2_pal_mem_api_t *h2_esp_board_dma_allocator(void);
const h2_pal_log_api_t *h2_esp_board_log_api(void);
const h2_pal_sync_api_t *h2_esp_board_sync_api(void);
const h2_pal_task_api_t *h2_esp_board_task_api(void);
const h2_pal_queue_api_t *h2_esp_board_queue_api(void);
const h2_pal_time_api_t *h2_esp_board_time_api(void);
const h2_pal_system_event_api_t *h2_esp_board_system_event_api(void);
const h2_pal_crypto_api_t *h2_esp_board_crypto_api(void);
const h2_pal_pref_api_t *h2_esp_board_pref_api(void);
const h2_pal_webrtc_api_t *h2_esp_board_webrtc_api(void);
h2_pal_ble_t *h2_esp_board_ble(void);
h2_pal_wifi_sta_t *h2_esp_board_wifi_sta(void);
h2_pal_wifi_ap_t *h2_esp_board_wifi_ap(void);
h2_pal_wifi_settings_t *h2_esp_board_wifi_settings(void);
const h2_pal_periph_api_t *h2_esp_board_periph_api(void);
const h2_pal_button_api_t *h2_esp_board_button_api(void);
h2_pal_input_api_t *h2_esp_board_input_api(void);
h2_pal_modem_t *h2_esp_board_modem(void);

h2_pal_display_t *h2_esp_board_display(void);
h2_pal_display_t *h2_esp_board_display_if_initialized(void);
h2_pal_audio_t *h2_esp_board_audio(void);
h2_pal_audio_t *h2_esp_board_audio_if_initialized(void);

#ifdef __cplusplus
}
#endif

#endif
