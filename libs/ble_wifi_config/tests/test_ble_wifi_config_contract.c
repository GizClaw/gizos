/*
 * Byte-exact conformance against the shared LiteLink wire-format vectors.
 *
 * The phone applications keep the same cases in
 * _Meta/contracts/wifi/scan_cases.json and provision_cases.json, checked by
 * flutter test/contracts/wifi_contract_test.dart and
 * wxmp/tests/contract-wifi.test.js. The vectors are copied here rather than
 * read from that repository: this package must build from a gizos checkout
 * alone. Update both copies together when the wire format changes.
 */

#include "h2_ble_wifi_config.h"
#include "h2_ble_wifi_config_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static size_t from_hex(const char *hex, uint8_t *out, size_t out_size) {
    size_t len = strlen(hex);
    CHECK(len % 2u == 0u);
    CHECK(len / 2u <= out_size);
    for (size_t i = 0u; i < len; i += 2u) {
        unsigned value = 0u;
        CHECK(sscanf(hex + i, "%2x", &value) == 1);
        out[i / 2u] = (uint8_t)value;
    }
    return len / 2u;
}

static void check_hex(const uint8_t *data, size_t len, const char *expected_hex) {
    uint8_t expected[256];
    size_t expected_len = from_hex(expected_hex, expected, sizeof(expected));
    CHECK(len == expected_len);
    CHECK(memcmp(data, expected, len) == 0);
}

typedef struct ap_case {
    const char *id;
    const char *ssid;
    int rssi;
    h2_pal_wifi_security_t security;
    const char *frame_hex;
} ap_case_t;

/* scan_cases.json frameCases, encoded from the device side. */
static const ap_case_t s_ap_cases[] = {
    { "ap_secure_strong", "HomeWiFi_2.4G", -45, H2_PAL_WIFI_SECURITY_WPA2,
      "01d3010d486f6d65576946695f322e3447" },
    { "ap_open", "CafeOpen", -55, H2_PAL_WIFI_SECURITY_OPEN,
      "01c90008436166654f70656e" },
    { "ap_utf8_ssid", "\xe5\xae\xa2\xe5\x8e\x85\xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8",
      -60, H2_PAL_WIFI_SECURITY_WPA3,
      "01c4010fe5aea2e58e85e8b7afe794b1e599a8" },
    { "ap_max_ssid", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", -70,
      H2_PAL_WIFI_SECURITY_WPA_WPA2,
      "01ba01204141414141414141414141414141414141414141414141414141414141414141" },
    { "ap_weak", "IoT_Network", -90, H2_PAL_WIFI_SECURITY_WPA2,
      "01a6010b496f545f4e6574776f726b" },
};

static void test_ap_frames(void) {
    for (size_t i = 0u; i < sizeof(s_ap_cases) / sizeof(s_ap_cases[0]); ++i) {
        const ap_case_t *entry_case = &s_ap_cases[i];
        h2_pal_wifi_scan_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        size_t ssid_len = strlen(entry_case->ssid);
        memcpy(entry.ssid, entry_case->ssid, ssid_len);
        entry.ssid_len = ssid_len;
        entry.rssi = entry_case->rssi;
        entry.security = entry_case->security;

        h2_ble_wifi_config_ap_t ap;
        CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
        uint8_t frame[H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN];
        size_t frame_len = 0u;
        CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &frame_len) ==
              H2_PAL_OK);
        check_hex(frame, frame_len, entry_case->frame_hex);
    }
}

static void test_scan_status_frames(void) {
    uint8_t frame[1];
    size_t frame_len = 0u;
    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED, frame, sizeof(frame),
              &frame_len) == H2_PAL_OK);
    check_hex(frame, frame_len, "02");
    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_ERROR, frame, sizeof(frame),
              &frame_len) == H2_PAL_OK);
    check_hex(frame, frame_len, "03");
}

/*
 * The applications drop these frames. The device must never produce one, so
 * the mirror of each malformed case is the entry the encoder refuses.
 */
static void test_unreportable_entries(void) {
    h2_pal_wifi_scan_entry_t entry;
    h2_ble_wifi_config_ap_t ap;

    /* malformed_zero_ssid_len: a hidden network. */
    memset(&entry, 0, sizeof(entry));
    entry.rssi = -45;
    entry.security = H2_PAL_WIFI_SECURITY_WPA2;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* malformed_ssid_len_overflow: 33 bytes. */
    memset(&entry, 0, sizeof(entry));
    memset(entry.ssid, 'A', H2_BLE_WIFI_CONFIG_SSID_MAX + 1u);
    entry.ssid_len = H2_BLE_WIFI_CONFIG_SSID_MAX + 1u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* malformed_invalid_utf8_ssid: 0xff cannot start a sequence. */
    memset(&entry, 0, sizeof(entry));
    entry.ssid[0] = (char)0xffu;
    entry.ssid_len = 1u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* malformed_truncated_utf8_ssid: e5 ae without its third byte. */
    memset(&entry, 0, sizeof(entry));
    entry.ssid[0] = (char)0xe5u;
    entry.ssid[1] = (char)0xaeu;
    entry.ssid_len = 2u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* malformed_overlong_utf8_ssid: c0 af encodes U+002F the long way. */
    memset(&entry, 0, sizeof(entry));
    entry.ssid[0] = (char)0xc0u;
    entry.ssid[1] = (char)0xafu;
    entry.ssid_len = 2u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* A surrogate encoded as three bytes is rejected as well. */
    memset(&entry, 0, sizeof(entry));
    entry.ssid[0] = (char)0xedu;
    entry.ssid[1] = (char)0xa0u;
    entry.ssid[2] = (char)0x80u;
    entry.ssid_len = 3u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);

    /* Valid four-byte sequences stay reportable. */
    memset(&entry, 0, sizeof(entry));
    entry.ssid[0] = (char)0xf0u;
    entry.ssid[1] = (char)0x9fu;
    entry.ssid[2] = (char)0x93u;
    entry.ssid[3] = (char)0xb6u;
    entry.ssid_len = 4u;
    entry.rssi = -50;
    entry.security = H2_PAL_WIFI_SECURITY_WPA2;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
}

typedef struct credentials_case {
    const char *id;
    const char *hex;
    const char *ssid;
    const char *password;
} credentials_case_t;

/* provision_cases.json credentialsCases, decoded on the device side. */
static const credentials_case_t s_credentials_cases[] = {
    { "secure_network", "0241420378797a", "AB", "xyz" },
    { "open_network", "014100", "A", "" },
    { "utf8_ssid", "06e5aea2e58e85027077", "\xe5\xae\xa2\xe5\x8e\x85", "pw" },
    { "max_sizes_fit_one_chunk",
      "2041414141414141414141414141414141414141414141414141414141414141413f"
      "7070707070707070707070707070707070707070707070707070707070707070707070"
      "70707070707070707070707070707070707070707070707070707070",
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      "ppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp" },
};

static void test_credentials(void) {
    for (size_t i = 0u; i < sizeof(s_credentials_cases) / sizeof(s_credentials_cases[0]);
         ++i) {
        const credentials_case_t *credentials_case = &s_credentials_cases[i];
        uint8_t frame[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN];
        size_t frame_len = from_hex(credentials_case->hex, frame, sizeof(frame));
        h2_ble_wifi_config_credentials_t credentials;
        CHECK(h2_ble_wifi_config_decode_credentials(frame, frame_len, &credentials) ==
              H2_PAL_OK);
        CHECK(credentials.ssid_len == strlen(credentials_case->ssid));
        CHECK(memcmp(credentials.ssid, credentials_case->ssid,
                     credentials.ssid_len) == 0);
        CHECK(credentials.password_len == strlen(credentials_case->password));
        CHECK(memcmp(credentials.password, credentials_case->password,
                     credentials.password_len) == 0);
    }
    /* The longest legal frame is 97 bytes, below the 100-byte MTU floor. */
    uint8_t frame[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN];
    CHECK(from_hex(s_credentials_cases[3].hex, frame, sizeof(frame)) == 97u);
}

typedef struct result_case {
    const char *id;
    const char *hex;
    h2_ble_wifi_config_status_t status;
    h2_ble_wifi_config_reason_t reason;
} result_case_t;

/* provision_cases.json resultCases, encoded on the device side. */
static const result_case_t s_result_cases[] = {
    { "success", "0000", H2_BLE_WIFI_CONFIG_STATUS_SUCCESS,
      H2_BLE_WIFI_CONFIG_REASON_NONE },
    { "wrong_password", "0101", H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
      H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD },
    { "ap_not_found", "0102", H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
      H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND },
    { "dhcp_failed", "0103", H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
      H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED },
    { "timeout", "0104", H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
      H2_BLE_WIFI_CONFIG_REASON_CONNECT_TIMEOUT },
    { "unknown", "01ff", H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
      H2_BLE_WIFI_CONFIG_REASON_UNKNOWN },
};

static void test_results(void) {
    for (size_t i = 0u; i < sizeof(s_result_cases) / sizeof(s_result_cases[0]); ++i) {
        const result_case_t *result_case = &s_result_cases[i];
        uint8_t frame[H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN];
        size_t frame_len = 0u;
        CHECK(h2_ble_wifi_config_encode_result(
                  result_case->status, result_case->reason, frame, sizeof(frame),
                  &frame_len) == H2_PAL_OK);
        check_hex(frame, frame_len, result_case->hex);
    }
}

static void test_command_opcodes(void) {
    h2_ble_wifi_config_opcode_t opcode;
    const uint8_t start[] = { 0x01u };
    CHECK(h2_ble_wifi_config_decode_command(start, sizeof(start), &opcode) == H2_PAL_OK);
    CHECK(opcode == H2_BLE_WIFI_CONFIG_OPCODE_SCAN_START);
    const uint8_t stop[] = { 0x02u };
    CHECK(h2_ble_wifi_config_decode_command(stop, sizeof(stop), &opcode) == H2_PAL_OK);
    CHECK(opcode == H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP);
}

/*
 * The applications hard-code the printed UUIDs; the service stores them in
 * ATT byte order, which is the reverse. A transcription slip here would only
 * show up as a device that never matches on the phone.
 */
static void test_default_uuids(void) {
    struct {
        const uint8_t *uuid;
        const char *printed;
    } cases[] = {
        { h2_ble_wifi_config_default_service_uuid,
          "bdda0001ca524b138f17b1e139bd5d1a" },
        { h2_ble_wifi_config_default_command_uuid,
          "bdda0002ca524b138f17b1e139bd5d1a" },
        { h2_ble_wifi_config_default_scan_uuid,
          "bdda0003ca524b138f17b1e139bd5d1a" },
        { h2_ble_wifi_config_default_provision_uuid,
          "bdda0004ca524b138f17b1e139bd5d1a" },
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t printed[16];
        CHECK(from_hex(cases[i].printed, printed, sizeof(printed)) == 16u);
        for (size_t j = 0u; j < 16u; ++j) {
            CHECK(cases[i].uuid[j] == printed[15u - j]);
        }
    }
}

static void test_mtu_floor(void) {
    CHECK(H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN == 97u);
    CHECK(H2_BLE_WIFI_CONFIG_MIN_ATT_MTU == 100u);
    CHECK(H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN == 36u);
}

int main(void) {
    test_ap_frames();
    test_scan_status_frames();
    test_unreportable_entries();
    test_credentials();
    test_results();
    test_command_opcodes();
    test_default_uuids();
    test_mtu_floor();
    printf("ok\n");
    return 0;
}
