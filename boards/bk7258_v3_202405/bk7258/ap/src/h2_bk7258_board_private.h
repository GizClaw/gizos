#ifndef H2_BK7258_BOARD_PRIVATE_H
#define H2_BK7258_BOARD_PRIVATE_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/hal/h2_pal_touch.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/hal/h2_pal_input.h"
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/hal/h2_pal_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BK7258_BOARD_NAME "bk7258_v3_202405"
#define H2_BK7258_CHIP_NAME "bk7258"

#define H2_BK7258_FLASHDB_KV_OFFSET 0x780000u
#define H2_BK7258_FLASHDB_KV_SIZE (128u * 1024u)
#define H2_BK7258_COREDUMP_OFFSET 0x7a0000u
#define H2_BK7258_COREDUMP_SIZE (360u * 1024u)
#define H2_BK7258_FATFS_DRIVE "1:"
#define H2_BK7258_SD_ROOT "1:/h2loader"
#define H2_BK7258_SD_DL_ROOT "1:/h2loader/dl"
#define H2_BK7258_SD_DATA_ROOT "1:/h2loader/data"
#define H2_BK7258_DL_MOUNT_PATH "/dl"
#define H2_BK7258_DATA_MOUNT_PATH "/data"
#define H2_BK7258_ADC_BUTTON_GROUP_ID 100u

typedef struct h2_bk7258_audio_config {
    uint32_t sample_rate;
    uint16_t frame_samples_per_channel;
    uint8_t channels;
    uint8_t mic_channels;
    uint8_t speaker_channels;
    uint8_t bits_per_sample;
    uint8_t default_volume;
    uint8_t default_mic_gain;
    uint8_t frame_count;
} h2_bk7258_audio_config_t;

typedef struct h2_bk7258_button_adc_range {
    uint8_t id;
    uint16_t min_mv;
    uint16_t max_mv;
} h2_bk7258_button_adc_range_t;

extern const h2_bk7258_audio_config_t h2_bk7258_audio_config;
extern const h2_bk7258_button_adc_range_t h2_bk7258_button_ranges[];
extern const size_t h2_bk7258_button_range_count;

h2_pal_mem_api_t *h2_bk7258_board_default_allocator(void);
h2_pal_mem_api_t *h2_bk7258_board_sram_allocator(void);
h2_pal_mem_api_t *h2_bk7258_board_psram_allocator(void);
const h2_pal_log_api_t *h2_bk7258_board_log_api(void);
const h2_pal_sync_api_t *h2_bk7258_board_sync_api(void);
const h2_pal_task_api_t *h2_bk7258_board_task_api(void);
const h2_pal_queue_api_t *h2_bk7258_board_queue_api(void);
const h2_pal_time_api_t *h2_bk7258_board_time_api(void);
const h2_pal_system_event_api_t *h2_bk7258_board_system_event_api(void);
const h2_pal_crypto_api_t *h2_bk7258_board_crypto_api(void);
const h2_pal_pref_api_t *h2_bk7258_board_pref_api(void);
const h2_pal_webrtc_api_t *h2_bk7258_board_webrtc_api(void);
h2_pal_ble_t *h2_bk7258_board_ble(void);
h2_pal_wifi_sta_t *h2_bk7258_board_wifi_sta(void);
h2_pal_wifi_ap_t *h2_bk7258_board_wifi_ap(void);
h2_pal_wifi_settings_t *h2_bk7258_board_wifi_settings(void);
const h2_pal_periph_api_t *h2_bk7258_board_periph_api(void);
const h2_pal_button_api_t *h2_bk7258_board_button_api(void);
const h2_pal_touch_api_t *h2_bk7258_board_touch_api(void);
h2_pal_input_api_t *h2_bk7258_board_input_api(void);
h2_pal_modem_t *h2_bk7258_board_modem(void);
h2_pal_display_t *h2_bk7258_board_display(void);
h2_pal_audio_t *h2_bk7258_board_audio(void);
int h2_bk7258_board_display_black(void);
int h2_bk7258_board_fs_init(h2_pal_fs_api_t *fs);

#ifdef __cplusplus
}
#endif

#endif
