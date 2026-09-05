#include "h2_ble_wifi_config_protocol.h"

#include <stdbool.h>
#include <string.h>

/*
 * IEEE 802.11 and ESP-IDF station disconnect reasons. The Wi-Fi PAL passes
 * the backend value through unchanged, and every station backend in this
 * repository reports these numbers today.
 */
#define H2_BLE_WIFI_CONFIG_DISCONNECT_AUTH_EXPIRE 2
#define H2_BLE_WIFI_CONFIG_DISCONNECT_4WAY_TIMEOUT 15
#define H2_BLE_WIFI_CONFIG_DISCONNECT_802_1X_FAILED 23
#define H2_BLE_WIFI_CONFIG_DISCONNECT_BEACON_TIMEOUT 200
#define H2_BLE_WIFI_CONFIG_DISCONNECT_NO_AP_FOUND 201
#define H2_BLE_WIFI_CONFIG_DISCONNECT_AUTH_FAIL 202
#define H2_BLE_WIFI_CONFIG_DISCONNECT_ASSOC_FAIL 203
#define H2_BLE_WIFI_CONFIG_DISCONNECT_HANDSHAKE_TIMEOUT 204

int h2_ble_wifi_config_decode_command(
    const uint8_t *data,
    size_t len,
    h2_ble_wifi_config_opcode_t *out_opcode) {
    if (data == NULL || out_opcode == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len != H2_BLE_WIFI_CONFIG_COMMAND_FRAME_LEN) {
        return H2_PAL_ERR_FORMAT;
    }
    switch (data[0]) {
    case (uint8_t)H2_BLE_WIFI_CONFIG_OPCODE_SCAN_START:
        *out_opcode = H2_BLE_WIFI_CONFIG_OPCODE_SCAN_START;
        return H2_PAL_OK;
    case (uint8_t)H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP:
        *out_opcode = H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP;
        return H2_PAL_OK;
    default:
        return H2_PAL_ERR_FORMAT;
    }
}

int h2_ble_wifi_config_encode_ap(
    const h2_ble_wifi_config_ap_t *ap,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ap == NULL || out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ap->ssid_len == 0u || ap->ssid_len > H2_BLE_WIFI_CONFIG_SSID_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t len = 4u + ap->ssid_len;
    if (out_size < len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    out[0] = (uint8_t)H2_BLE_WIFI_CONFIG_SCAN_FRAME_AP;
    out[1] = (uint8_t)ap->rssi;
    out[2] = ap->flags;
    out[3] = (uint8_t)ap->ssid_len;
    memcpy(out + 4, ap->ssid, ap->ssid_len);
    *out_len = len;
    return H2_PAL_OK;
}

int h2_ble_wifi_config_encode_scan_status(
    h2_ble_wifi_config_scan_frame_t frame,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (frame != H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED &&
        frame != H2_BLE_WIFI_CONFIG_SCAN_FRAME_ERROR) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_size < H2_BLE_WIFI_CONFIG_SCAN_STATUS_FRAME_LEN) {
        return H2_PAL_ERR_NO_SPACE;
    }
    out[0] = (uint8_t)frame;
    *out_len = H2_BLE_WIFI_CONFIG_SCAN_STATUS_FRAME_LEN;
    return H2_PAL_OK;
}

int h2_ble_wifi_config_decode_credentials(
    const uint8_t *data,
    size_t len,
    h2_ble_wifi_config_credentials_t *out_credentials) {
    if (out_credentials != NULL) {
        memset(out_credentials, 0, sizeof(*out_credentials));
    }
    if (data == NULL || out_credentials == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len < 2u || len > H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t ssid_len = data[0];
    if (ssid_len == 0u || ssid_len > H2_BLE_WIFI_CONFIG_SSID_MAX ||
        len < 2u + ssid_len) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t password_len = data[1u + ssid_len];
    if (password_len > H2_BLE_WIFI_CONFIG_PASSWORD_MAX ||
        len != 2u + ssid_len + password_len) {
        return H2_PAL_ERR_FORMAT;
    }
    memcpy(out_credentials->ssid, data + 1, ssid_len);
    out_credentials->ssid_len = ssid_len;
    memcpy(out_credentials->password, data + 2 + ssid_len, password_len);
    out_credentials->password_len = password_len;
    return H2_PAL_OK;
}

int h2_ble_wifi_config_encode_result(
    h2_ble_wifi_config_status_t status,
    h2_ble_wifi_config_reason_t reason,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_size < H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN) {
        return H2_PAL_ERR_NO_SPACE;
    }
    out[0] = (uint8_t)H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL;
    out[1] = (uint8_t)status;
    out[2] = (uint8_t)reason;
    *out_len = H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN;
    return H2_PAL_OK;
}

int h2_ble_wifi_config_encode_progress(
    h2_ble_wifi_config_progress_t state,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_size < H2_BLE_WIFI_CONFIG_PROGRESS_FRAME_LEN) {
        return H2_PAL_ERR_NO_SPACE;
    }
    out[0] = (uint8_t)H2_BLE_WIFI_CONFIG_PROVISION_FRAME_PROGRESS;
    out[1] = (uint8_t)state;
    *out_len = H2_BLE_WIFI_CONFIG_PROGRESS_FRAME_LEN;
    return H2_PAL_OK;
}

/**
 * Strict UTF-8 validation, matching what the phone applications accept.
 *
 * A beacon SSID is arbitrary bytes. The applications decode it strictly and
 * drop the whole frame when that fails, so an access point with a non-UTF-8
 * SSID would silently vanish from the list. Dropping it here instead keeps
 * both sides on the same rule and counts it.
 *
 * Overlong encodings, surrogates and code points above U+10FFFF are rejected.
 */
static bool h2_ble_wifi_config_is_valid_utf8(const char *data, size_t len) {
    size_t i = 0u;
    while (i < len) {
        uint8_t byte = (uint8_t)data[i];
        size_t extra;
        uint32_t min_code_point;
        uint32_t code_point;
        if (byte < 0x80u) {
            i++;
            continue;
        }
        if (byte >= 0xc2u && byte <= 0xdfu) {
            extra = 1u;
            min_code_point = 0x80u;
            code_point = (uint32_t)(byte & 0x1fu);
        } else if (byte >= 0xe0u && byte <= 0xefu) {
            extra = 2u;
            min_code_point = 0x800u;
            code_point = (uint32_t)(byte & 0x0fu);
        } else if (byte >= 0xf0u && byte <= 0xf4u) {
            extra = 3u;
            min_code_point = 0x10000u;
            code_point = (uint32_t)(byte & 0x07u);
        } else {
            /* 0x80-0xc1 and 0xf5-0xff cannot start a sequence. */
            return false;
        }
        if (len - i <= extra) {
            return false;
        }
        for (size_t j = 1u; j <= extra; ++j) {
            uint8_t continuation = (uint8_t)data[i + j];
            if (continuation < 0x80u || continuation > 0xbfu) {
                return false;
            }
            code_point = (code_point << 6) | (uint32_t)(continuation & 0x3fu);
        }
        if (code_point < min_code_point || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return false;
        }
        i += extra + 1u;
    }
    return true;
}

int h2_ble_wifi_config_ap_from_scan_entry(
    const h2_pal_wifi_scan_entry_t *entry,
    h2_ble_wifi_config_ap_t *out_ap) {
    if (out_ap != NULL) {
        memset(out_ap, 0, sizeof(*out_ap));
    }
    if (entry == NULL || out_ap == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Hidden and over-long SSIDs have no representation on the wire. */
    if (entry->ssid_len == 0u || entry->ssid_len > H2_BLE_WIFI_CONFIG_SSID_MAX) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (!h2_ble_wifi_config_is_valid_utf8(entry->ssid, entry->ssid_len)) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memcpy(out_ap->ssid, entry->ssid, entry->ssid_len);
    out_ap->ssid_len = entry->ssid_len;
    int rssi = entry->rssi;
    if (rssi > INT8_MAX) {
        rssi = INT8_MAX;
    } else if (rssi < INT8_MIN) {
        rssi = INT8_MIN;
    }
    out_ap->rssi = (int8_t)rssi;
    /*
     * An unknown security type is reported as secured: prompting for a
     * passphrase that turns out to be unnecessary is recoverable, silently
     * offering an open join for a protected network is not.
     */
    out_ap->flags = entry->security == H2_PAL_WIFI_SECURITY_OPEN
                        ? 0u
                        : H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED;
    return H2_PAL_OK;
}

h2_ble_wifi_config_reason_t h2_ble_wifi_config_default_reason(
    int connect_result,
    const h2_pal_wifi_sta_status_t *status) {
    int disconnect_reason = status != NULL ? status->disconnect_reason : 0;
    switch (disconnect_reason) {
    case H2_BLE_WIFI_CONFIG_DISCONNECT_AUTH_EXPIRE:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_4WAY_TIMEOUT:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_802_1X_FAILED:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_AUTH_FAIL:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_ASSOC_FAIL:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_HANDSHAKE_TIMEOUT:
        return H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD;
    case H2_BLE_WIFI_CONFIG_DISCONNECT_BEACON_TIMEOUT:
    case H2_BLE_WIFI_CONFIG_DISCONNECT_NO_AP_FOUND:
        return H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND;
    default:
        break;
    }
    if (status != NULL &&
        (status->state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
         status->state == H2_PAL_WIFI_STA_STATE_GOT_IP) &&
        status->ip_valid == 0u) {
        /* Associated without an address: the lease failed, not the key. */
        return H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED;
    }
    if (connect_result == H2_PAL_ERR_TIMEOUT) {
        return H2_BLE_WIFI_CONFIG_REASON_CONNECT_TIMEOUT;
    }
    return H2_BLE_WIFI_CONFIG_REASON_UNKNOWN;
}
