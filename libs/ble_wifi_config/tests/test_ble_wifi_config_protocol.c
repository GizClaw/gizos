#include "h2_ble_wifi_config_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static void fill_ssid(char *out, size_t len, char seed) {
    for (size_t i = 0u; i < len; ++i) {
        out[i] = (char)(seed + (char)(i % 26));
    }
}

static void test_decode_command(void) {
    h2_ble_wifi_config_opcode_t opcode = (h2_ble_wifi_config_opcode_t)0;
    const uint8_t start[] = { 0x01u };
    CHECK(h2_ble_wifi_config_decode_command(start, sizeof(start), &opcode) == H2_PAL_OK);
    CHECK(opcode == H2_BLE_WIFI_CONFIG_OPCODE_SCAN_START);

    const uint8_t stop[] = { 0x02u };
    CHECK(h2_ble_wifi_config_decode_command(stop, sizeof(stop), &opcode) == H2_PAL_OK);
    CHECK(opcode == H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP);

    const uint8_t unknown[] = { 0x00u };
    CHECK(h2_ble_wifi_config_decode_command(unknown, sizeof(unknown), &opcode) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t reserved[] = { 0xffu };
    CHECK(h2_ble_wifi_config_decode_command(reserved, sizeof(reserved), &opcode) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t too_long[] = { 0x01u, 0x01u };
    CHECK(h2_ble_wifi_config_decode_command(too_long, sizeof(too_long), &opcode) ==
          H2_PAL_ERR_FORMAT);
    CHECK(h2_ble_wifi_config_decode_command(start, 0u, &opcode) == H2_PAL_ERR_FORMAT);
    CHECK(h2_ble_wifi_config_decode_command(NULL, 1u, &opcode) == H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_decode_command(start, 1u, NULL) == H2_PAL_ERR_INVALID_ARG);
}

static void test_encode_ap(void) {
    uint8_t frame[H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN];
    size_t len = 0u;
    h2_ble_wifi_config_ap_t ap;

    memset(&ap, 0, sizeof(ap));
    ap.ssid[0] = 'a';
    ap.ssid_len = 1u;
    ap.rssi = -45;
    ap.flags = H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED;
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &len) == H2_PAL_OK);
    CHECK(len == 5u);
    CHECK(frame[0] == 0x01u);
    /* -45 is 0xd3 in two's complement. */
    CHECK(frame[1] == 0xd3u);
    CHECK(frame[2] == 0x01u);
    CHECK(frame[3] == 0x01u);
    CHECK(frame[4] == 'a');

    memset(&ap, 0, sizeof(ap));
    fill_ssid(ap.ssid, H2_BLE_WIFI_CONFIG_SSID_MAX, 'A');
    ap.ssid_len = H2_BLE_WIFI_CONFIG_SSID_MAX;
    ap.rssi = 0;
    ap.flags = 0u;
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &len) == H2_PAL_OK);
    CHECK(len == H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN);
    CHECK(frame[3] == (uint8_t)H2_BLE_WIFI_CONFIG_SSID_MAX);
    CHECK(memcmp(frame + 4, ap.ssid, H2_BLE_WIFI_CONFIG_SSID_MAX) == 0);

    /* One byte short of the encoded frame must fail without writing. */
    len = 1u;
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame) - 1u, &len) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(len == 0u);

    ap.ssid_len = 0u;
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &len) ==
          H2_PAL_ERR_INVALID_ARG);
    ap.ssid_len = H2_BLE_WIFI_CONFIG_SSID_MAX + 1u;
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &len) ==
          H2_PAL_ERR_INVALID_ARG);
    ap.ssid_len = 4u;
    CHECK(h2_ble_wifi_config_encode_ap(NULL, frame, sizeof(frame), &len) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_encode_ap(&ap, NULL, sizeof(frame), &len) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), NULL) ==
          H2_PAL_ERR_INVALID_ARG);
}

static void test_encode_scan_status(void) {
    uint8_t frame[2] = { 0xaau, 0xaau };
    size_t len = 0u;
    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED, frame, sizeof(frame), &len) ==
          H2_PAL_OK);
    CHECK(len == 1u);
    CHECK(frame[0] == 0x02u);
    /* A status frame carries no field beyond its type. */
    CHECK(frame[1] == 0xaau);

    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_ERROR, frame, sizeof(frame), &len) ==
          H2_PAL_OK);
    CHECK(len == 1u);
    CHECK(frame[0] == 0x03u);

    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_AP, frame, sizeof(frame), &len) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_encode_scan_status(
              H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED, frame, 0u, &len) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(len == 0u);
}

static void test_decode_credentials(void) {
    h2_ble_wifi_config_credentials_t credentials;
    uint8_t frame[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN + 4u];

    /* ssid_len = 1, pass_len = 0. */
    frame[0] = 1u;
    frame[1] = 'x';
    frame[2] = 0u;
    CHECK(h2_ble_wifi_config_decode_credentials(frame, 3u, &credentials) == H2_PAL_OK);
    CHECK(credentials.ssid_len == 1u);
    CHECK(credentials.ssid[0] == 'x');
    CHECK(credentials.ssid[1] == '\0');
    CHECK(credentials.password_len == 0u);
    CHECK(credentials.password[0] == '\0');

    /* ssid_len = 32, pass_len = 63: the longest legal frame. */
    frame[0] = (uint8_t)H2_BLE_WIFI_CONFIG_SSID_MAX;
    for (size_t i = 0u; i < H2_BLE_WIFI_CONFIG_SSID_MAX; ++i) {
        frame[1u + i] = (uint8_t)('A' + (int)(i % 26u));
    }
    frame[1u + H2_BLE_WIFI_CONFIG_SSID_MAX] = (uint8_t)H2_BLE_WIFI_CONFIG_PASSWORD_MAX;
    for (size_t i = 0u; i < H2_BLE_WIFI_CONFIG_PASSWORD_MAX; ++i) {
        frame[2u + H2_BLE_WIFI_CONFIG_SSID_MAX + i] = (uint8_t)('0' + (int)(i % 10u));
    }
    CHECK(h2_ble_wifi_config_decode_credentials(
              frame, H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN, &credentials) ==
          H2_PAL_OK);
    CHECK(credentials.ssid_len == H2_BLE_WIFI_CONFIG_SSID_MAX);
    CHECK(credentials.password_len == H2_BLE_WIFI_CONFIG_PASSWORD_MAX);
    CHECK(credentials.ssid[H2_BLE_WIFI_CONFIG_SSID_MAX] == '\0');
    CHECK(credentials.password[H2_BLE_WIFI_CONFIG_PASSWORD_MAX] == '\0');

    /* A passphrase one byte over the protocol limit is rejected. */
    frame[1u + H2_BLE_WIFI_CONFIG_SSID_MAX] =
        (uint8_t)(H2_BLE_WIFI_CONFIG_PASSWORD_MAX + 1u);
    CHECK(h2_ble_wifi_config_decode_credentials(
              frame, H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN + 1u,
              &credentials) == H2_PAL_ERR_FORMAT);
    CHECK(credentials.ssid_len == 0u);

    /* Hidden SSID, truncation, trailing bytes and an over-long SSID. */
    const uint8_t hidden[] = { 0u, 0u };
    CHECK(h2_ble_wifi_config_decode_credentials(hidden, sizeof(hidden), &credentials) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t truncated[] = { 4u, 'a', 'b' };
    CHECK(h2_ble_wifi_config_decode_credentials(
              truncated, sizeof(truncated), &credentials) == H2_PAL_ERR_FORMAT);
    const uint8_t missing_pass_len[] = { 2u, 'a', 'b' };
    CHECK(h2_ble_wifi_config_decode_credentials(
              missing_pass_len, sizeof(missing_pass_len), &credentials) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t trailing[] = { 1u, 'a', 0u, 0xffu };
    CHECK(h2_ble_wifi_config_decode_credentials(trailing, sizeof(trailing), &credentials) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t short_pass[] = { 1u, 'a', 2u, 'p' };
    CHECK(h2_ble_wifi_config_decode_credentials(short_pass, sizeof(short_pass), &credentials) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t long_ssid[] = { 33u, 'a' };
    CHECK(h2_ble_wifi_config_decode_credentials(long_ssid, sizeof(long_ssid), &credentials) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t empty[] = { 1u };
    CHECK(h2_ble_wifi_config_decode_credentials(empty, 1u, &credentials) ==
          H2_PAL_ERR_FORMAT);
    CHECK(h2_ble_wifi_config_decode_credentials(empty, 0u, &credentials) ==
          H2_PAL_ERR_FORMAT);
    CHECK(h2_ble_wifi_config_decode_credentials(NULL, 3u, &credentials) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_decode_credentials(frame, 3u, NULL) ==
          H2_PAL_ERR_INVALID_ARG);
}

static void test_decode_credentials_fuzz(void) {
    /*
     * Every length and every leading byte combination must terminate with a
     * defined result and must not read past the frame.
     */
    uint8_t frame[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN + 8u];
    h2_ble_wifi_config_credentials_t credentials;
    for (size_t len = 0u; len <= sizeof(frame); ++len) {
        for (unsigned pattern = 0u; pattern < 256u; ++pattern) {
            memset(frame, (int)pattern, sizeof(frame));
            int rc = h2_ble_wifi_config_decode_credentials(frame, len, &credentials);
            CHECK(rc == H2_PAL_OK || rc == H2_PAL_ERR_FORMAT);
            if (rc == H2_PAL_OK) {
                CHECK(credentials.ssid_len >= 1u);
                CHECK(credentials.ssid_len <= H2_BLE_WIFI_CONFIG_SSID_MAX);
                CHECK(credentials.password_len <= H2_BLE_WIFI_CONFIG_PASSWORD_MAX);
                CHECK(len == 2u + credentials.ssid_len + credentials.password_len);
            }
        }
    }
}

static void test_encode_progress(void) {
    uint8_t frame[H2_BLE_WIFI_CONFIG_PROGRESS_FRAME_LEN];
    size_t len = 0u;
    CHECK(h2_ble_wifi_config_encode_progress(
              H2_BLE_WIFI_CONFIG_PROGRESS_ASSOCIATING, frame, sizeof(frame),
              &len) == H2_PAL_OK);
    CHECK(len == 2u);
    CHECK(frame[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_PROGRESS);
    CHECK(frame[1] == 0x01u);

    CHECK(h2_ble_wifi_config_encode_progress(
              H2_BLE_WIFI_CONFIG_PROGRESS_ADDRESS_ACQUIRED, frame,
              sizeof(frame), &len) == H2_PAL_OK);
    CHECK(frame[1] == 0x03u);

    /* A progress frame never overlaps a final frame's length. */
    CHECK(len != H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN);

    len = 1u;
    CHECK(h2_ble_wifi_config_encode_progress(
              H2_BLE_WIFI_CONFIG_PROGRESS_ASSOCIATED, frame, 1u, &len) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(len == 0u);
}

static void test_encode_result(void) {
    uint8_t frame[H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN];
    size_t len = 0u;
    CHECK(h2_ble_wifi_config_encode_result(
              H2_BLE_WIFI_CONFIG_STATUS_SUCCESS, H2_BLE_WIFI_CONFIG_REASON_NONE,
              frame, sizeof(frame), &len) == H2_PAL_OK);
    CHECK(len == 3u);
    CHECK(frame[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(frame[1] == 0x00u);
    CHECK(frame[2] == 0x00u);

    CHECK(h2_ble_wifi_config_encode_result(
              H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
              H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD, frame, sizeof(frame), &len) ==
          H2_PAL_OK);
    CHECK(frame[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(frame[1] == 0x01u);
    CHECK(frame[2] == 0x01u);

    CHECK(h2_ble_wifi_config_encode_result(
              H2_BLE_WIFI_CONFIG_STATUS_FAILURE, H2_BLE_WIFI_CONFIG_REASON_UNKNOWN,
              frame, sizeof(frame), &len) == H2_PAL_OK);
    CHECK(frame[2] == 0xffu);

    CHECK(h2_ble_wifi_config_encode_result(
              H2_BLE_WIFI_CONFIG_STATUS_SUCCESS, H2_BLE_WIFI_CONFIG_REASON_NONE,
              frame, 1u, &len) == H2_PAL_ERR_NO_SPACE);
    CHECK(len == 0u);
}

static void test_ap_from_scan_entry(void) {
    h2_pal_wifi_scan_entry_t entry;
    h2_ble_wifi_config_ap_t ap;

    memset(&entry, 0, sizeof(entry));
    memcpy(entry.ssid, "home", 4);
    entry.ssid_len = 4u;
    entry.rssi = -70;
    entry.security = H2_PAL_WIFI_SECURITY_WPA2;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
    CHECK(ap.ssid_len == 4u);
    CHECK(memcmp(ap.ssid, "home", 4) == 0);
    CHECK(ap.rssi == -70);
    CHECK(ap.flags == H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED);

    entry.security = H2_PAL_WIFI_SECURITY_OPEN;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
    CHECK(ap.flags == 0u);

    /* Unknown security is reported as secured, never as an open network. */
    entry.security = H2_PAL_WIFI_SECURITY_UNKNOWN;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
    CHECK(ap.flags == H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED);

    /* Out-of-range RSSI clamps into the signed byte. */
    entry.rssi = 1000;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
    CHECK(ap.rssi == 127);
    entry.rssi = -1000;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_OK);
    CHECK(ap.rssi == -128);

    /* Hidden networks are not reported. */
    entry.ssid_len = 0u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);
    CHECK(ap.ssid_len == 0u);

    entry.ssid_len = H2_BLE_WIFI_CONFIG_SSID_MAX + 1u;
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, &ap) == H2_PAL_ERR_NOT_FOUND);
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(NULL, &ap) == H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_ap_from_scan_entry(&entry, NULL) == H2_PAL_ERR_INVALID_ARG);
}

static void test_default_reason(void) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));

    status.disconnect_reason = 15; /* 4-way handshake timeout. */
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_TIMEOUT, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD);
    status.disconnect_reason = 202; /* ESP auth fail. */
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_IO, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD);
    status.disconnect_reason = 201; /* ESP no AP found. */
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_IO, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND);

    status.disconnect_reason = 0;
    status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
    status.ip_valid = 0u;
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_TIMEOUT, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED);

    status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_TIMEOUT, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_CONNECT_TIMEOUT);
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_IO, &status) ==
          H2_BLE_WIFI_CONFIG_REASON_UNKNOWN);
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_IO, NULL) ==
          H2_BLE_WIFI_CONFIG_REASON_UNKNOWN);
    CHECK(h2_ble_wifi_config_default_reason(H2_PAL_ERR_TIMEOUT, NULL) ==
          H2_BLE_WIFI_CONFIG_REASON_CONNECT_TIMEOUT);
}

int main(void) {
    test_decode_command();
    test_encode_ap();
    test_encode_scan_status();
    test_decode_credentials();
    test_decode_credentials_fuzz();
    test_encode_progress();
    test_encode_result();
    test_ap_from_scan_entry();
    test_default_reason();
    printf("ok\n");
    return 0;
}
