#ifndef H2_BLE_WIFI_CONFIG_PROTOCOL_H
#define H2_BLE_WIFI_CONFIG_PROTOCOL_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief BLE Wi-Fi provisioning wire format.
 *
 * The phone application writes one command or one credential set per ATT
 * write, and the device sends one access point per ATT notification. Every
 * frame fits in a single ATT value, so the protocol has no fragmentation
 * layer, no sequence numbers and no acknowledgements. All multi-byte values
 * are little endian; the current frames carry no multi-byte field.
 *
 * Command characteristic, device <- application, exactly one byte:
 *
 *     u8  opcode      0x01 = start scan, 0x02 = stop scan
 *
 * Scan characteristic, device -> application:
 *
 *     u8  type        0x01 = access point, 0x02 = scan finished,
 *                     0x03 = scan error
 *     i8  rssi        dBm, two's complement, -45 encodes as 0xd3
 *     u8  flags       bit0: 1 = secured, 0 = open; other bits reserved zero
 *     u8  ssid_len    1..32
 *     u8  ssid[ssid_len]  UTF-8 without a trailing NUL
 *
 * Type 0x02 and 0x03 frames are one byte long and carry no further field. A
 * type 0x01 frame is at most 4 + 32 = 36 bytes. Hidden networks are not
 * reported.
 *
 * Provisioning characteristic, device <- application:
 *
 *     u8  ssid_len    1..32
 *     u8  ssid[ssid_len]
 *     u8  pass_len    0..63, zero for an open network
 *     u8  pass[pass_len]
 *
 * A credential frame is at most 2 + 32 + 63 = 97 bytes, so the negotiated
 * ATT MTU must be at least 100 bytes.
 *
 * Provisioning characteristic, device -> application, after the connection
 * attempt has finished:
 *
 *     u8  status      0x00 = success, nonzero = failure
 *     u8  reason      0x00 = none, 0x01 = wrong password,
 *                     0x02 = access point not found, 0x03 = DHCP failed,
 *                     0x04 = connect timeout, 0xff = unknown error
 */

/** Longest SSID the protocol can carry, in bytes. */
#define H2_BLE_WIFI_CONFIG_SSID_MAX 32u
/** Longest passphrase the protocol can carry, in bytes. */
#define H2_BLE_WIFI_CONFIG_PASSWORD_MAX 63u
/** Shortest negotiated ATT MTU that can carry a full credential frame. */
#define H2_BLE_WIFI_CONFIG_MIN_ATT_MTU 100u
/** Length of a command frame, in bytes. */
#define H2_BLE_WIFI_CONFIG_COMMAND_FRAME_LEN 1u
/** Length of a scan frame that only carries its type, in bytes. */
#define H2_BLE_WIFI_CONFIG_SCAN_STATUS_FRAME_LEN 1u
/** Longest access-point scan frame, in bytes. */
#define H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN (4u + H2_BLE_WIFI_CONFIG_SSID_MAX)
/** Longest credential frame, in bytes. */
#define H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN \
    (2u + H2_BLE_WIFI_CONFIG_SSID_MAX + H2_BLE_WIFI_CONFIG_PASSWORD_MAX)
/** Length of a provisioning result frame, in bytes. */
#define H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN 2u

_Static_assert(
    H2_BLE_WIFI_CONFIG_SSID_MAX <= (unsigned)H2_PAL_WIFI_SSID_MAX,
    "protocol SSID limit must fit the Wi-Fi PAL SSID limit");
_Static_assert(
    H2_BLE_WIFI_CONFIG_PASSWORD_MAX <= (unsigned)H2_PAL_WIFI_PASSWORD_MAX,
    "protocol passphrase limit must fit the Wi-Fi PAL passphrase limit");

/** Command opcodes carried by the command characteristic. */
typedef enum h2_ble_wifi_config_opcode {
    H2_BLE_WIFI_CONFIG_OPCODE_SCAN_START = 0x01,
    H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP = 0x02,
} h2_ble_wifi_config_opcode_t;

/** First byte of a scan notification. */
typedef enum h2_ble_wifi_config_scan_frame {
    H2_BLE_WIFI_CONFIG_SCAN_FRAME_AP = 0x01,
    H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED = 0x02,
    H2_BLE_WIFI_CONFIG_SCAN_FRAME_ERROR = 0x03,
} h2_ble_wifi_config_scan_frame_t;

/** Access point is not open. */
#define H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED ((uint8_t)(1u << 0))

/** Status byte of a provisioning result frame. */
typedef enum h2_ble_wifi_config_status {
    H2_BLE_WIFI_CONFIG_STATUS_SUCCESS = 0x00,
    H2_BLE_WIFI_CONFIG_STATUS_FAILURE = 0x01,
} h2_ble_wifi_config_status_t;

/** Reason byte of a provisioning result frame. */
typedef enum h2_ble_wifi_config_reason {
    H2_BLE_WIFI_CONFIG_REASON_NONE = 0x00,
    H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD = 0x01,
    H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND = 0x02,
    H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED = 0x03,
    H2_BLE_WIFI_CONFIG_REASON_CONNECT_TIMEOUT = 0x04,
    H2_BLE_WIFI_CONFIG_REASON_UNKNOWN = 0xff,
} h2_ble_wifi_config_reason_t;

/** One reported access point, already reduced to protocol fields. */
typedef struct h2_ble_wifi_config_ap {
    /** NUL-terminated for convenience; ssid_len is authoritative. */
    char ssid[H2_BLE_WIFI_CONFIG_SSID_MAX + 1u];
    size_t ssid_len;
    int8_t rssi;
    uint8_t flags;
} h2_ble_wifi_config_ap_t;

/** One decoded credential frame. */
typedef struct h2_ble_wifi_config_credentials {
    /** NUL-terminated for convenience; ssid_len is authoritative. */
    char ssid[H2_BLE_WIFI_CONFIG_SSID_MAX + 1u];
    size_t ssid_len;
    /** NUL-terminated for convenience; password_len is authoritative. */
    char password[H2_BLE_WIFI_CONFIG_PASSWORD_MAX + 1u];
    size_t password_len;
} h2_ble_wifi_config_credentials_t;

/**
 * @brief Decode a command frame.
 *
 * @param data Borrowed frame bytes.
 * @param len Frame length in bytes; exactly one byte is accepted.
 * @param out_opcode Receives the opcode on success and is otherwise
 * untouched.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL argument, or
 * H2_PAL_ERR_FORMAT for a wrong length or an unknown opcode.
 */
int h2_ble_wifi_config_decode_command(
    const uint8_t *data,
    size_t len,
    h2_ble_wifi_config_opcode_t *out_opcode);

/**
 * @brief Encode an access-point scan frame.
 *
 * @param ap Borrowed access point with an SSID length of 1..32.
 * @param out Caller-provided output buffer.
 * @param out_size Capacity of @p out in bytes.
 * @param out_len Set to zero first, then to the encoded length on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL argument or an
 * out-of-range SSID length, or H2_PAL_ERR_NO_SPACE when @p out_size is too
 * small.
 */
int h2_ble_wifi_config_encode_ap(
    const h2_ble_wifi_config_ap_t *ap,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);

/**
 * @brief Encode a scan-finished or scan-error frame.
 *
 * @param frame Frame type; only the one-byte status frames are accepted.
 * @param out Caller-provided output buffer.
 * @param out_size Capacity of @p out in bytes.
 * @param out_len Set to zero first, then to the encoded length on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL pointer or a frame
 * type that carries fields, or H2_PAL_ERR_NO_SPACE when @p out_size is zero.
 */
int h2_ble_wifi_config_encode_scan_status(
    h2_ble_wifi_config_scan_frame_t frame,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);

/**
 * @brief Decode a credential frame.
 *
 * The frame must be exactly as long as its declared fields; trailing bytes
 * are rejected instead of ignored.
 *
 * @param data Borrowed frame bytes.
 * @param len Frame length in bytes.
 * @param out_credentials Cleared first, then filled on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL argument, or
 * H2_PAL_ERR_FORMAT for a truncated, over-long or out-of-range frame.
 */
int h2_ble_wifi_config_decode_credentials(
    const uint8_t *data,
    size_t len,
    h2_ble_wifi_config_credentials_t *out_credentials);

/**
 * @brief Encode a provisioning result frame.
 *
 * @param status Success or failure.
 * @param reason Failure reason, or H2_BLE_WIFI_CONFIG_REASON_NONE.
 * @param out Caller-provided output buffer.
 * @param out_size Capacity of @p out in bytes.
 * @param out_len Set to zero first, then to the encoded length on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL pointer, or
 * H2_PAL_ERR_NO_SPACE when @p out_size is smaller than two bytes.
 */
int h2_ble_wifi_config_encode_result(
    h2_ble_wifi_config_status_t status,
    h2_ble_wifi_config_reason_t reason,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);

/**
 * @brief Reduce a Wi-Fi PAL scan entry to protocol access-point fields.
 *
 * @param entry Borrowed scan entry.
 * @param out_ap Cleared first, then filled on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG for a NULL argument, or
 * H2_PAL_ERR_NOT_FOUND for a hidden or over-long SSID that must not be
 * reported.
 */
int h2_ble_wifi_config_ap_from_scan_entry(
    const h2_pal_wifi_scan_entry_t *entry,
    h2_ble_wifi_config_ap_t *out_ap);

/**
 * @brief Map a station status to a provisioning failure reason.
 *
 * The default mapping reads the IEEE 802.11 and ESP-IDF reason codes that
 * every supported station backend reports today. Providers whose
 * disconnect_reason uses another numbering supply their own mapping through
 * h2_ble_wifi_config_config_t::map_reason.
 *
 * @param connect_result Result returned by h2_pal_wifi_sta_connect().
 * @param status Borrowed station status read after the attempt, or NULL when
 * the status could not be read.
 * @return The reason byte to report, never
 * H2_BLE_WIFI_CONFIG_REASON_NONE.
 */
h2_ble_wifi_config_reason_t h2_ble_wifi_config_default_reason(
    int connect_result,
    const h2_pal_wifi_sta_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
