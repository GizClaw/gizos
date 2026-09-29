#include "h2_pal_wifi_device.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

const char h2_wifi_device_runner_task_name[] = "pal-wifi/e2e/runner";

static h2_wifi_e2e_result_t result;
static int run_rc, persistence, backup_cleared;
static const char *image_version, *board_name;
static uint32_t boot;
static uint64_t nonce;

typedef struct backup {
    uint32_t has;
    h2_pal_wifi_sta_config_t config;
} backup_t;
static int close_ns(h2_pal_pref_namespace_t *ns, int rc) {
    int next = ns->close(ns);
    return rc ? rc : next;
}
static int get_current(h2_runtime_t *rt, backup_t *b) {
    memset(b, 0, sizeof(*b));
    int has = 0;
    int rc = h2_pal_wifi_settings_has_saved_sta_config(rt->wifi_settings, &has);
    if (!rc)
        b->has = (uint32_t)has;
    if (!rc && has)
        rc = h2_pal_wifi_settings_get_saved_sta_config(rt->wifi_settings, &b->config);
    if (!rc &&
        (has < 0 || has > 1 || (has && h2_pal_wifi_settings_validate_sta_config(&b->config))))
        rc = H2_PAL_ERR_FORMAT;
    return rc;
}
static void encode(const backup_t *b, uint8_t data[108]) {
    memset(data, 0, 108);
    data[0] = (uint8_t)b->has;
    data[1] = (uint8_t)b->config.ssid_len;
    data[2] = (uint8_t)b->config.password_len;
    data[3] = b->config.bssid_set;
    data[4] = b->config.channel;
    memcpy(data + 5, b->config.bssid, 6);
    memcpy(data + 11, b->config.ssid, b->config.ssid_len);
    memcpy(data + 43, b->config.password, b->config.password_len);
    data[107] = 1;
}
static int decode(const uint8_t *data, size_t length, backup_t *b) {
    if (length != 108 || data[107] != 1 || data[0] > 1 || data[1] > 32 || data[2] > 64 ||
        data[3] > 1)
        return H2_PAL_ERR_FORMAT;
    memset(b, 0, sizeof(*b));
    b->has = data[0];
    b->config.ssid_len = data[1];
    b->config.password_len = data[2];
    b->config.bssid_set = data[3];
    b->config.channel = data[4];
    memcpy(b->config.bssid, data + 5, 6);
    memcpy(b->config.ssid, data + 11, data[1]);
    memcpy(b->config.password, data + 43, data[2]);
    return b->has && h2_pal_wifi_settings_validate_sta_config(&b->config) ? H2_PAL_ERR_FORMAT
                                                                          : H2_PAL_OK;
}
static int digest(h2_runtime_t *rt, const backup_t *b, uint8_t out[32]) {
    /* Canonical fixed-width record, independent of ABI padding. No credential
     * or digest bytes enter a report. This is a persistence oracle, not a
     * password-authentication protocol. */
    uint8_t data[108];
    encode(b, data);
    static const uint8_t salt[] = "pal-wifi-persistence-v1";
    int rc = h2_pal_crypto_hkdf_sha256(rt->crypto, data, sizeof(data), salt, sizeof(salt) - 1, NULL,
                                       0, out, 32);
    memset(data, 0, sizeof(data));
    return rc;
}
static int prepare(h2_runtime_t *rt, const char *version) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = h2_pal_pref_open(rt->pref, "h2wifictl", H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc)
        return rc;
    void *old = NULL;
    size_t length = 0;
    int recovered = 0;
    rc = ns->get_blob(ns, rt->mem, "backup", &old, &length);
    if (!rc) {
        backup_t backup;
        rc = decode(old, length, &backup);
        if (!rc) {
            rc = backup.has
                     ? h2_pal_wifi_settings_set_saved_sta_config(rt->wifi_settings, &backup.config)
                     : h2_pal_wifi_settings_clear_saved_sta_config(rt->wifi_settings);
            recovered = !rc;
        }
        memset(&backup, 0, sizeof(backup));
    } else if (rc == H2_PAL_ERR_NOT_FOUND)
        rc = H2_PAL_OK;
    h2_pal_mem_free(rt->mem, old);
    if (rc)
        return close_ns(ns, rc);
    backup_t current;
    rc = get_current(rt, &current);
    if (rc)
        return close_ns(ns, rc);
    uint8_t actual[32];
    rc = digest(rt, &current, actual);
    if (rc)
        return close_ns(ns, rc);
    char *previous_version = NULL;
    old = NULL;
    length = 0;
    int read = ns->get_string(ns, rt->mem, "version", &previous_version);
    persistence = 0;
    if (!read && !strcmp(previous_version, version)) {
        read = ns->get_blob(ns, rt->mem, "expected", &old, &length);
        if (!read && length == 32 && !memcmp(old, actual, 32) && !recovered)
            persistence = 1;
        else if (!recovered)
            rc = H2_PAL_ERR_IO;
    } else if (read != H2_PAL_OK && read != H2_PAL_ERR_NOT_FOUND)
        rc = read;
    h2_pal_mem_free(rt->mem, previous_version);
    h2_pal_mem_free(rt->mem, old);
    uint32_t previous_boot = 0;
    read = ns->get_u32(ns, "boot", &previous_boot);
    if (read && read != H2_PAL_ERR_NOT_FOUND && !rc)
        rc = read;
    boot = previous_boot + 1;
    uint8_t encoded[108];
    encode(&current, encoded);
    if (!rc)
        rc = ns->set_blob(ns, "backup", encoded, sizeof(encoded));
    if (!rc)
        rc = ns->set_u32(ns, "boot", boot);
    if (!rc)
        rc = ns->commit(ns);
    memset(&current, 0, sizeof(current));
    memset(actual, 0, sizeof(actual));
    memset(encoded, 0, sizeof(encoded));
    printf("H2_WIFI_RECOVERY recovered=%d rc=%d\n", recovered, rc);
    return close_ns(ns, rc);
}
static int finish(h2_runtime_t *rt) {
    backup_t current;
    int rc = get_current(rt, &current);
    uint8_t actual[32];
    if (!rc)
        rc = digest(rt, &current, actual);
    if (rc)
        return rc;
    h2_pal_pref_namespace_t *ns = NULL;
    rc = h2_pal_pref_open(rt->pref, "h2wifictl", H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (!rc) {
        rc = ns->set_blob(ns, "expected", actual, sizeof(actual));
        if (!rc)
            rc = ns->set_string(ns, "version", image_version);
        if (!rc)
            rc = ns->remove(ns, "backup");
        if (!rc)
            rc = ns->commit(ns);
        rc = close_ns(ns, rc);
    }
    backup_cleared = !rc;
    memset(&current, 0, sizeof(current));
    memset(actual, 0, sizeof(actual));
    return rc;
}
static void record(void *user, const char *id, int rc, uint64_t elapsed) {
    (void)user;
    const char *status = !rc                                                            ? "PASS"
                         : rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_UNAVAILABLE ? "BLOCKED"
                                                                                        : "FAIL";
    if (rc == H2_PAL_ERR_WOULD_BLOCK && !strcmp(id, "settings-restart-persistence"))
        status = "PENDING";
    printf("H2_WIFI_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"elapsed_ms\":%" PRIu64
           ",\"line\":%u,\"boot\":%lu,\"nonce\":%" PRIu64 "}\n",
           id, status, rc, elapsed, result.last_error_line, (unsigned long)boot, nonce);
    fflush(stdout);
}
int h2_wifi_device_run(h2_runtime_t *rt, const char *version, const char *board) {
    if (!rt || !version || !board)
        return H2_PAL_ERR_INVALID_ARG;
    image_version = version;
    board_name = board;
    backup_cleared = 0;
    int rc = h2_pal_crypto_random(rt->crypto, (uint8_t *)&nonce, sizeof(nonce));
    if (!rc)
        rc = prepare(rt, version);
    if (rc)
        return rc;
    printf("H2_WIFI_BOOT {\"board\":\"%s\",\"version\":\"%s\",\"boot\":%lu,\"nonce\":%" PRIu64
           ",\"persistence\":%d}\n",
           board, version, (unsigned long)boot, nonce, persistence);
    record(NULL, "settings-restart-persistence", persistence ? 0 : H2_PAL_ERR_WOULD_BLOCK, 0);
    h2_wifi_e2e_config_t cfg = {
        .operation_timeout_ms = 30000, .client_timeout_ms = 45000, .case_result = record};
    memcpy(cfg.fixture.ssid, "h2wifi-fixture", 14);
    cfg.fixture.ssid_len = 14;
    /* Public, isolated test AP key; never production provisioning material. */
    memcpy(cfg.fixture.password, "palwifie2e", 10);
    cfg.fixture.password_len = 10;
    cfg.fixture.channel = 6;
    const char *ssid = !strcmp(board, "bk7258") ? "h2wifi-dut-bk" : "h2wifi-dut-esp";
    cfg.ap.ssid_len = strlen(ssid);
    memcpy(cfg.ap.ssid, ssid, cfg.ap.ssid_len);
    memcpy(cfg.ap.password, "palwifie2e", 10);
    cfg.ap.password_len = 10;
    cfg.ap.channel = 6;
    cfg.ap.max_clients = 2;
    cfg.ap.security = H2_PAL_WIFI_SECURITY_WPA2;
    run_rc = h2_wifi_e2e_run(rt, &cfg, &result);
    if (!run_rc)
        run_rc = finish(rt);
    if (!run_rc && !persistence)
        run_rc = H2_PAL_ERR_WOULD_BLOCK;
    h2_wifi_device_report();
    return run_rc;
}
void h2_wifi_device_report(void) {
    printf("H2_WIFI_REPORT {\"board\":\"%s\",\"version\":\"%s\",\"boot\":%lu,\"nonce\":%" PRIu64
           ",\"operations\":21,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"persistence\":%d,"
           "\"cleanup\":%d,\"saved_restored\":%d,\"network_restored\":%d,\"backup_cleared\":%d,"
           "\"retained\":%u,\"qualified\":%d,\"rc\":%d,\"sta_connecting\":%u,\"sta_connected\":%u,"
           "\"sta_got_ip\":%u,\"sta_disconnected\":%u,\"ap_started\":%u,\"ap_stopped\":%u,\"client_"
           "joined\":%u,\"client_left\":%u,\"lease_granted\":%u,\"lease_released\":%u,\"route_"
           "changed\":%u,\"invalid_events\":%u,\"client_mac\":\"%02x%02x%02x%02x%02x%02x\","
           "\"client_ip4\":%lu}\n",
           board_name, image_version, (unsigned long)boot, nonce,
           result.passed + (unsigned)persistence, result.failed, result.blocked, persistence,
           result.cleanup_rc, result.saved_restored, result.network_restored, backup_cleared,
           result.retained, !run_rc && persistence && backup_cleared, run_rc, result.sta_connecting,
           result.sta_connected, result.sta_got_ip, result.sta_disconnected, result.ap_started,
           result.ap_stopped, result.client_joined, result.client_left, result.lease_granted,
           result.lease_released, result.route_changed, result.invalid_events, result.client_mac[0],
           result.client_mac[1], result.client_mac[2], result.client_mac[3], result.client_mac[4],
           result.client_mac[5], (unsigned long)result.client_ip4);
    fflush(stdout);
}
