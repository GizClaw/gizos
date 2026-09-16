#ifndef H2_WIFI_STA_H
#define H2_WIFI_STA_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* V1: 8-byte header, eight 110-byte records; all integers little-endian.
 * Header: magic "H2WN", u16 version=1, u8 count, u8 reserved=0.
 * Record: u8 SSID/password lengths, BSSID[6], u8 bssid_set/channel,
 * SSID[32], password[64], u32 last_connected_seq. Unused bytes are zero. */
#define H2_WIFI_SAVED_LIST_MAGIC UINT32_C(0x4e573248)
#define H2_WIFI_SAVED_LIST_RECORD_SIZE 110u
#define H2_WIFI_SAVED_LIST_BLOB_SIZE                                           \
  (8u + H2_PAL_WIFI_SAVED_NETWORK_MAX * H2_WIFI_SAVED_LIST_RECORD_SIZE)

/** Codec requires an exact fixed-size blob and space for eight decoded entries.
 * Malformed blobs return FORMAT without publishing a partial list.
 * Providers import their legacy record into slot zero before using this codec.
 */
int h2_wifi_saved_list_decode(const uint8_t *blob, size_t blob_size,
                              h2_pal_wifi_saved_network_t *out,
                              size_t *out_count);
int h2_wifi_saved_list_encode(const h2_pal_wifi_saved_network_t *saved,
                              size_t count, uint8_t *blob, size_t blob_size);
/** Mutate caller-owned eight-entry storage; count must be in [0, 8]. */
int h2_wifi_saved_list_insert(h2_pal_wifi_saved_network_t *saved, size_t *count,
                              const h2_pal_wifi_sta_config_t *config);
int h2_wifi_saved_list_remove(h2_pal_wifi_saved_network_t *saved, size_t *count,
                              const char *ssid, size_t ssid_len);
/** Pure ranking: strongest AP per SSID, descending RSSI then sequence.
 * Equal sequences preserve saved order. Output has room for eight configs.
 * Candidates retain credentials and use the scanned BSSID/channel. */
int h2_wifi_sta_rank_saved_candidates(const h2_pal_wifi_saved_network_t *saved,
                                      size_t saved_count,
                                      const h2_pal_wifi_scan_entry_t *scan,
                                      size_t scan_count,
                                      h2_pal_wifi_sta_config_t *out,
                                      size_t *out_count);

/** Borrowed provider dependencies, valid for the complete synchronous call. */
typedef struct h2_wifi_sta_dependencies {
    const h2_pal_wifi_sta_api_t *sta;
    const h2_pal_wifi_settings_api_t *settings;
    const h2_pal_time_api_t *time;
} h2_wifi_sta_dependencies_t;

/**
 * @brief Shared implementation of the PAL connect_and_save contract.
 * @param deps Raw provider operations, settings and monotonic clock.
 * @param config Borrowed target credentials; never retained.
 * @param timeout_ms Nonzero association/DHCP budget in milliseconds.
 * @return PAL result, including the storage failure without hiding it.
 * Provider must serialize connection mutations across this entire call and
 * supply raw operations that do not reacquire that admission gate. No task,
 * allocation or global state is created. Settings insertion must atomically
 * preserve the other saved networks.
 */
int h2_wifi_sta_connect_and_save(const h2_wifi_sta_dependencies_t *deps,
                                const h2_pal_wifi_sta_config_t *config,
                                uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
