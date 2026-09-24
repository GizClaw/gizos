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
#include "h2/pal/os/h2_pal_log.h"

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

/* FAT filesystem on the board SD slot, mapped to /dl and /data.
 * The composition root serializes init/deinit and stops all API consumers and
 * closes every file before deinit. An existing external mount is borrowed;
 * only a mount created here is unmounted, including failed-init cleanup.
 * Rename replaces a regular destination by delete-then-rename, not atomically;
 * identical mapped paths are a no-op after a successful open/close. Directory
 * destinations and directory opens return INVALID_STATE; cross-directory
 * rename returns UNSUPPORTED. Rename/remove/clear and truncating opens return
 * BUSY while another PAL handle holds the affected path (clear includes its
 * descendants); a read open also returns BUSY while a writer holds the path.
 * Two readers may coexist. The registry gate covers open/close and mutations;
 * direct SDK callers are outside this protection. Same-path rename only checks
 * existence and close status, even with an open PAL handle.
 * Translated paths allow 191 bytes plus NUL; the provider also enforces a
 * limit of 130 UTF-16 units per component, returning NO_SPACE before calling the
 * SDK to prevent silent name truncation. UTF-8 long names are encoded by SDK
 * fopen. On jlfat sync/f_free_cache is a successful no-op; native fclose writes
 * size and clusters to the card. */
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

typedef struct h2_jieli_ac791n_devkit_audio_idle {
  uint32_t open_tracks;         /* tracks not in the free state */
  uint32_t retained_operations; /* referenced writers, drainers, volume requests and callbacks */
  uint32_t ring_bytes;          /* PCM ring storage still allocated by tracks */
  uint32_t sdk_servers;         /* live encoder and decoder handles */
  uint32_t mic_open;            /* microphone session is not free */
  uint32_t speaker_started;
  uint64_t consumed_bytes;      /* PCM consumed from currently open tracks */
} h2_jieli_ac791n_devkit_audio_idle_t;

/* Snapshot under the provider gate; idle means all fields except consumed_bytes are zero. */
int h2_jieli_ac791n_devkit_audio_idle_probe(h2_jieli_ac791n_devkit_audio_idle_t *out);

/* BLE 5 peripheral Host with the H2Loader GATT schema, Extended Advertising,
 * DLE and MTU exchange. PAL PHY requests return UNSUPPORTED; the central
 * owns connection PHY selection. */
/* Borrows a valid firmware-lifetime Log capability. Repeated calls must use
 * the same object; invalid or replacement sinks return NULL. SDK diagnostic
 * records use DEBUG level; enablement/delivery belongs to the supplied sink. */
const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(const h2_pal_log_api_t *log);

/* On-chip 2.4 GHz Wi-Fi. Network-enabled layouts provide STA and AP modes;
 * compact layouts return explicit unsupported providers through Runtime.
 * Scans require STA mode (otherwise INVALID_STATE) and association (otherwise
 * SDK refusal returns BUSY). TIMEOUT leaves the scan SDK-owned: scan/connect
 * stay BUSY until completion is reaped or the interface is stopped. Disconnect,
 * AP stop and AP start admit this reset; successful wifi_off or the SDK's STA exit into
 * config mode (WIFI_EVENT_SMP_CFG_START) releases ownership; until that event
 * scan/connect stay BUSY.
 * After timeout, status reports the association state, never SCANNING. */
const h2_pal_wifi_sta_api_t *h2_jieli_ac791n_devkit_wifi_sta_api(void);
const h2_pal_wifi_ap_api_t *h2_jieli_ac791n_devkit_wifi_ap_api(void);
const h2_pal_wifi_settings_api_t *
h2_jieli_ac791n_devkit_wifi_settings_api(void);
/* lwIP full-duplex supports one reader, one writer and one closer; close
 * wakes blocked recv/send/connect. The provider returns BUSY for overlapping
 * same-direction operations or connect versus any transfer. Calls outside a
 * started PAL Wi-Fi interface and stale-generation descriptors are UNAVAILABLE.
 * Stop drains in-flight native operations; successful shutdown settles pending
 * DNS as UNAVAILABLE. A failed radio stop leaves the stack unavailable until a
 * later successful stop and start.
 * Close every socket and resolver before disconnect/ap_stop: descriptors left
 * across stop cannot be reclaimed safely, even by close (a no-op for stale fds).
 * DNS has four pending slots; TIMEOUT/WOULD_BLOCK leave a lookup pending.
 * There is no DNS cancellation: early close leaves backend ownership until
 * completion, or in a graveyard after stop until the next successful start.
 * Unique callback identities ignore late delivery even after address reuse. lwIP
 * owns retries (1, 1, 2, 3 seconds per server); the provider does not shorten them.
 * Synchronous DNS uses caller-owned address storage, not a shared hostent. */
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
