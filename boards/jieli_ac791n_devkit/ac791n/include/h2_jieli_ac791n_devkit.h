#ifndef H2_JIELI_AC791N_DEVKIT_H
#define H2_JIELI_AC791N_DEVKIT_H

#include <stddef.h>

#include "h2/pal/hal/h2_pal_button.h"
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/hal/h2_pal_touch.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_pref.h"

typedef struct h2_runtime_config h2_runtime_config_t;
typedef struct h2_pal_wifi_sta_api h2_pal_wifi_sta_api_t;
typedef struct h2_pal_wifi_ap_api h2_pal_wifi_ap_api_t;
typedef struct h2_pal_wifi_settings_api h2_pal_wifi_settings_api_t;
typedef struct h2_pal_net_api h2_pal_net_api_t;
typedef struct h2_pal_netif_api h2_pal_netif_api_t;

#ifdef __cplusplus
extern "C" {
#endif

const char *h2_jieli_ac791n_devkit_board_name(void);
/** Persisted BLE identity address, formatted as 12 lowercase hex digits. */
const char *h2_jieli_ac791n_devkit_device_uid(void);

/* Start the board USB0 CDC/debug endpoint once. Board early-init owns the
 * first call; Loader and App transports only verify/reuse it. */
h2_pal_result_t h2_jieli_ac791n_devkit_usb_debug_start(void);

/* Shared layout console. Writes are atomic with respect to printf batches
 * and protocol frames. Read is nonblocking, single-reader; errors are PAL. */
h2_pal_result_t h2_jieli_ac791n_devkit_console_start(void);
int h2_jieli_ac791n_devkit_console_read(void *buffer, size_t size);
int h2_jieli_ac791n_devkit_console_write(
    const void *buffer, size_t size, uint32_t timeout_ms);

/* Raw on-chip NOR partitions owned by the physical board layout. */
const h2_pal_disk_api_t *h2_jieli_ac791n_devkit_disk_api(void);

/* FAT filesystem on the board SD slot, mapped to /dl and /data. */
h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_init(h2_pal_fs_api_t *out_api);
h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_deinit(void);
const char *h2_jieli_ac791n_devkit_sd_fs_last_stage(void);
int h2_jieli_ac791n_devkit_sd_fs_diagnostic(
    char *out, size_t out_size);

/* Typed preferences stored in a 256 KiB LittleFS backing store. */
const h2_pal_pref_api_t *h2_jieli_ac791n_devkit_pref_api(void);
void h2_jieli_ac791n_devkit_pref_set_diagnostic(
    void (*write_line)(const char *line));

/* 320x480 ILI9481/ILI9488 panel selected by the board strap pins. */
const h2_pal_display_api_t *h2_jieli_ac791n_devkit_display_api(void);

/* FT6236 single-pointer touch controller on the board software-I2C bus. */
const h2_pal_touch_api_t *h2_jieli_ac791n_devkit_touch_api(void);

enum {
  H2_JIELI_AC791N_ADKEY_GROUP_ID = 1,
  H2_JIELI_AC791N_ADKEY_POWER_ID = 2,
  H2_JIELI_AC791N_ADKEY_ENCODER_ID = 3,
  H2_JIELI_AC791N_ADKEY_PHOTO_ID = 4,
  H2_JIELI_AC791N_ADKEY_OK_ID = 5,
  H2_JIELI_AC791N_ADKEY_VOLUME_UP_ID = 6,
  H2_JIELI_AC791N_ADKEY_VOLUME_DOWN_ID = 7,
  H2_JIELI_AC791N_ADKEY_MODE_ID = 8,
  H2_JIELI_AC791N_ADKEY_CANCEL_ID = 9,
};

/* Eight resistor-ladder keys sampled on PB1/AD_CH_PB01. */
const h2_pal_button_api_t *h2_jieli_ac791n_devkit_button_api(void);

/* On-chip MIC1 ADC and DAC/PA on the development board. */
const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void);

/* BLE 5 peripheral Host with the H2Loader GATT schema, Extended Advertising,
 * DLE, MTU exchange and 2M/Coded PHY requests. */
const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(void);

/* On-chip 2.4 GHz Wi-Fi. Network-enabled layouts provide STA and AP modes;
 * compact layouts return explicit unsupported providers through Runtime. */
const h2_pal_wifi_sta_api_t *h2_jieli_ac791n_devkit_wifi_sta_api(void);
const h2_pal_wifi_ap_api_t *h2_jieli_ac791n_devkit_wifi_ap_api(void);
const h2_pal_wifi_settings_api_t *
h2_jieli_ac791n_devkit_wifi_settings_api(void);
const h2_pal_net_api_t *h2_jieli_ac791n_devkit_net_api(void);
const h2_pal_netif_api_t *h2_jieli_ac791n_devkit_netif_api(void);

/* Complete Runtime provider table for this physical board. */
h2_pal_result_t h2_jieli_ac791n_devkit_runtime_config(
    h2_runtime_config_t *out_config);
h2_pal_result_t h2_jieli_ac791n_devkit_runtime_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
