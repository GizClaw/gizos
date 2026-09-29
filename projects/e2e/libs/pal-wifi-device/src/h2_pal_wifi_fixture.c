#include "h2_pal_wifi_device.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static h2_runtime_t *log_runtime;
static int log_error;
static void emit(const char *format, ...) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    int rc = length < 0 || (size_t)length >= sizeof(message)
                 ? H2_PAL_ERR_TRUNCATED
                 : h2_pal_log_write(log_runtime->log, H2_PAL_LOG_INFO, "pal-wifi", message);
    if (rc && !log_error)
        log_error = rc;
}

static uint64_t time_ms(h2_runtime_t *rt) {
    uint64_t t = 0;
    (void)h2_pal_time_get_monotonic_ms(rt->time, &t);
    return t;
}
static void drain(h2_runtime_t *rt) {
    union {
        uint64_t align;
        unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    } payload;
    h2_runtime_event_t event = {.payload = payload.bytes,
                                .payload_capacity = sizeof(payload.bytes)};
    while (h2_runtime_poll_event(rt, &event) == H2_PAL_OK) {
    }
}
typedef struct fixture_scan {
    h2_pal_wifi_scan_entry_t entry;
    const char *ssid;
    int found;
} fixture_scan_t;
static bool result(void *user, const h2_pal_wifi_scan_entry_t *e) {
    fixture_scan_t *s = user;
    if (e && e->ssid_len == strlen(s->ssid) && !memcmp(e->ssid, s->ssid, e->ssid_len)) {
        s->entry = *e;
        s->found = 1;
        return false;
    }
    return true;
}
int h2_wifi_fixture_run(h2_runtime_t *rt) {
    log_runtime = rt;
    log_error = 0;
    h2_pal_wifi_sta_config_t original = {0};
    int has = 0;
    int rc = h2_pal_wifi_settings_has_saved_sta_config(rt->wifi_settings, &has);
    if (!rc && has)
        rc = h2_pal_wifi_settings_get_saved_sta_config(rt->wifi_settings, &original);
    if (rc)
        return rc;
    h2_pal_wifi_ap_config_t ap = {.ssid = "h2wifi-fixture",
                                  .ssid_len = 14,
                                  .password = "palwifie2e",
                                  .password_len = 10,
                                  .channel = 6,
                                  .max_clients = 4,
                                  .security = H2_PAL_WIFI_SECURITY_WPA2};
    uint8_t mac[6] = {0};
    rc = h2_pal_wifi_sta_get_mac(rt->wifi_sta, mac);
    if (!rc)
        rc = h2_pal_wifi_ap_start(rt->wifi_ap, &ap, 30000);
    if (rc)
        return rc;
    emit("H2_WIFI_FIXTURE_START window_ms=600000 sta_mac=%02x%02x%02x%02x%02x%02x\n", mac[0],
         mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint64_t start = time_ms(rt);
    unsigned joins = 0;
    const char *names[] = {"h2wifi-dut-esp", "h2wifi-dut-bk"};
    while (time_ms(rt) - start < 600000) {
        for (unsigned i = 0; i < 2 && time_ms(rt) - start < 600000; ++i) {
            drain(rt);
            fixture_scan_t scan = {.ssid = names[i]};
            h2_pal_wifi_scan_request_t request = {.channel = 6};
            request.ssid_len = strlen(names[i]);
            memcpy(request.ssid, names[i], request.ssid_len);
            int scan_rc = h2_pal_wifi_sta_scan(rt->wifi_sta, &request, result, &scan, 10000);
            if (scan_rc)
                continue;
            h2_pal_wifi_sta_config_t config = {.channel = 6};
            config.ssid_len = strlen(names[i]);
            memcpy(config.ssid, names[i], config.ssid_len);
            /* Known directed credentials also exercise a hidden DUT AP that
             * some SDK scanners omit even from their directed scan list. */
            if (!scan.found || scan.entry.security != H2_PAL_WIFI_SECURITY_OPEN) {
                memcpy(config.password, "palwifie2e", 10);
                config.password_len = 10;
            }
            (void)h2_pal_wifi_sta_disconnect(rt->wifi_sta);
            drain(rt);
            int connect_rc = h2_pal_wifi_sta_connect(rt->wifi_sta, &config, 15000);
            uint64_t wait = time_ms(rt);
            h2_pal_wifi_sta_status_t status = {0};
            while (!connect_rc && time_ms(rt) - wait < 15000) {
                connect_rc = h2_pal_wifi_sta_get_status(rt->wifi_sta, &status);
                if (status.ip_valid)
                    break;
                (void)h2_pal_time_sleep_ms(rt->time, 100);
            }
            if (!connect_rc && status.ip_valid) {
                ++joins;
                emit("H2_WIFI_FIXTURE_CLIENT target=%s joined=%u ip4=%lu "
                     "mac=%02x%02x%02x%02x%02x%02x\n",
                     names[i], joins, (unsigned long)status.ip.ip4, mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);

                (void)h2_pal_time_sleep_ms(rt->time, 10000);
            }
            (void)h2_pal_wifi_sta_disconnect(rt->wifi_sta);
            emit("H2_WIFI_FIXTURE_LEFT target=%s rc=%d\n", names[i], connect_rc);

            (void)h2_pal_time_sleep_ms(rt->time, 2500);
        }
    }
    int cleanup = h2_pal_wifi_ap_stop(rt->wifi_ap, 30000);
    int next = h2_pal_wifi_sta_disconnect(rt->wifi_sta);
    if (!cleanup)
        cleanup = next;
    int settings_unchanged = 0;
    h2_pal_wifi_sta_config_t saved = {0};
    int current_has = 0;
    next = h2_pal_wifi_settings_has_saved_sta_config(rt->wifi_settings, &current_has);
    if (!next && current_has == has) {
        if (has)
            next = h2_pal_wifi_settings_get_saved_sta_config(rt->wifi_settings, &saved);
        settings_unchanged = !next && (!has || h2_wifi_config_equal(&saved, &original));
    }
    if (!settings_unchanged && !cleanup)
        cleanup = H2_PAL_ERR_IO;
    int network_restored = !has;
    if (has) {
        next = h2_pal_wifi_sta_connect(rt->wifi_sta, &original, 30000);
        if (!cleanup)
            cleanup = next;
        uint64_t wait = time_ms(rt);
        h2_pal_wifi_sta_status_t status = {0};
        while (!next && time_ms(rt) - wait < 30000) {
            next = h2_pal_wifi_sta_get_status(rt->wifi_sta, &status);
            if (status.ip_valid) {
                network_restored = 1;
                break;
            }
            (void)h2_pal_time_sleep_ms(rt->time, 100);
        }
        if (!network_restored && !cleanup)
            cleanup = H2_PAL_ERR_TIMEOUT;
    }
    emit("H2_WIFI_FIXTURE_END joins=%u cleanup=%d settings_unchanged=%d network_restored=%d\n",
         joins, cleanup, settings_unchanged, network_restored);

    memset(&original, 0, sizeof(original));
    memset(&saved, 0, sizeof(saved));
    return cleanup ? cleanup : log_error;
}
