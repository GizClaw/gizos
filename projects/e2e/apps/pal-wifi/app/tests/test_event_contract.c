#include "h2/pal/h2_pal_unsupported.h"
#include "../src/h2_pal_wifi_e2e.c"

#include <assert.h>

static void station(wifi_test_t *s, h2_runtime_event_kind_t kind,
                    h2_runtime_system_wifi_sta_status_t status, int has_ip) {
    h2_runtime_system_event_wifi_sta_t value = {0};
    value.status = status;
    value.ssid_len = s->cfg->fixture.ssid_len;
    memcpy(value.ssid, s->cfg->fixture.ssid, value.ssid_len);
    value.ip_valid = has_ip;
    if (has_ip) {
        value.ip.ip4 = 0xC0A80402u;
        value.ip.netmask4 = 0xFFFFFF00u;
        value.ip.gateway4 = 0xC0A80401u;
        value.bssid[0] = 0x02;
        value.bssid[5] = 0x01;
        value.bssid_set = 1;
        value.channel = 6;
    }
    h2_runtime_event_t event = {.kind = kind, .component = H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
                                .payload = &value, .payload_size = sizeof(value)};
    observe_event(s, &event);
}

static void client(wifi_test_t *s, h2_runtime_event_kind_t kind, int lease,
                   uint32_t ip) {
    h2_runtime_system_event_wifi_ap_client_t value = {0};
    value.mac[0] = 0x02;
    value.mac[5] = 0x01;
    value.lease_valid = lease;
    value.lease.ip4 = ip;
    h2_runtime_event_t event = {.kind = kind, .component = H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
                                .payload = &value, .payload_size = sizeof(value)};
    observe_event(s, &event);
}

static void access_point(wifi_test_t *s, h2_runtime_event_kind_t kind,
                         h2_runtime_system_wifi_ap_status_t status) {
    h2_runtime_system_event_wifi_ap_t value = {.status = status, .ssid_len = 3};
    memcpy(value.ssid, "dut", 3);
    h2_runtime_event_t event = {.kind = kind, .component = H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
                                .payload = &value, .payload_size = sizeof(value)};
    observe_event(s, &event);
}

typedef struct cleanup_fixture {
    uint64_t now;
    unsigned starts, stops;
    unsigned scans;
    int malformed_scan;
    int active;
    h2_pal_wifi_ap_config_t config;
} cleanup_fixture_t;

static int cleanup_sta_disconnect(void *user) {
    (void)user;
    return H2_PAL_OK;
}
static int cleanup_ap_start(void *user, const h2_pal_wifi_ap_config_t *config,
                            uint32_t timeout_ms) {
    (void)timeout_ms;
    cleanup_fixture_t *fixture = user;
    ++fixture->starts;
    fixture->active = 1;
    fixture->config = *config;
    return H2_PAL_OK;
}
static int cleanup_ap_stop(void *user, uint32_t timeout_ms) {
    (void)timeout_ms;
    cleanup_fixture_t *fixture = user;
    ++fixture->stops;
    fixture->active = 0;
    return H2_PAL_OK;
}
static int cleanup_ap_status(void *user, h2_pal_wifi_ap_status_t *out) {
    cleanup_fixture_t *fixture = user;
    memset(out, 0, sizeof(*out));
    out->state = fixture->active ? H2_PAL_WIFI_AP_STATE_STARTED : H2_PAL_WIFI_AP_STATE_STOPPED;
    out->security = fixture->config.security;
    out->hidden = fixture->config.hidden;
    return H2_PAL_OK;
}
static int cleanup_ap_clients(void *user, h2_pal_wifi_ap_client_t *out, size_t capacity,
                              size_t *count) {
    (void)user;
    (void)out;
    (void)capacity;
    *count = 0;
    return H2_PAL_OK;
}
static int cleanup_time(void *user, uint64_t *out) {
    *out = ++((cleanup_fixture_t *)user)->now;
    return H2_PAL_OK;
}
static int cleanup_sleep(void *user, uint32_t ms) {
    ((cleanup_fixture_t *)user)->now += ms;
    return H2_PAL_OK;
}
static int scan_after_fixture_start(void *user, const h2_pal_wifi_scan_request_t *request,
                                    h2_pal_wifi_scan_result_fn callback, void *callback_user,
                                    uint32_t timeout_ms) {
    (void)request;
    (void)timeout_ms;
    cleanup_fixture_t *fixture = user;
    ++fixture->scans;
    h2_pal_wifi_scan_entry_t entry = {0};
    const char *ssid = fixture->scans == 1u ? "other" : "fixture";
    entry.ssid_len = strlen(ssid);
    memcpy(entry.ssid, ssid, entry.ssid_len);
    entry.bssid[0] = fixture->malformed_scan ? 0x01u : 0x02u;
    entry.bssid[5] = 1u;
    entry.channel = 6u;
    entry.rssi = -45;
    (void)callback(callback_user, &entry);
    return H2_PAL_OK;
}
static void test_scan_waits_for_target_without_masking_malformed_records(void) {
    cleanup_fixture_t fixture = {0};
    const h2_pal_wifi_sta_vtable_t sta_v = {.scan = scan_after_fixture_start};
    const h2_pal_time_vtable_t time_v = {
        .get_monotonic_ms = cleanup_time, .sleep_ms = cleanup_sleep};
    const h2_pal_wifi_sta_api_t sta = {&fixture, &sta_v};
    const h2_pal_time_api_t time = {&fixture, &time_v};
    h2_runtime_t runtime = {.wifi_sta = &sta, .time = &time};
    h2_wifi_e2e_config_t config = {
        .fixture = {.ssid = "fixture", .ssid_len = 7, .channel = 6},
        .operation_timeout_ms = 3000};
    wifi_test_t state = {.rt = &runtime, .cfg = &config};
    assert(scan_test(&state, 0, 0) == H2_PAL_OK);
    assert(fixture.scans == 2u && state.fixture_channel == 6u);
    fixture.scans = 0u;
    fixture.malformed_scan = 1;
    assert(scan_test(&state, 0, 0) == H2_PAL_ERR_IO);
    assert(fixture.scans == 1u);
}
static void test_failed_ap_mode_still_stops_ap(void) {
    cleanup_fixture_t fixture = {0};
    const h2_pal_wifi_sta_vtable_t sta_v = {.disconnect = cleanup_sta_disconnect};
    const h2_pal_wifi_ap_vtable_t ap_v = {
        .start = cleanup_ap_start, .stop = cleanup_ap_stop,
        .get_status = cleanup_ap_status, .get_clients = cleanup_ap_clients};
    const h2_pal_time_vtable_t time_v = {
        .get_monotonic_ms = cleanup_time, .sleep_ms = cleanup_sleep};
    const h2_pal_wifi_sta_api_t sta = {&fixture, &sta_v};
    const h2_pal_wifi_ap_api_t ap = {&fixture, &ap_v};
    const h2_pal_time_api_t time = {&fixture, &time_v};
    h2_runtime_t runtime = {.wifi_sta = &sta, .wifi_ap = &ap,
                            .netif = h2_pal_unsupported_netif_api(), .time = &time};
    h2_wifi_e2e_config_t config = {
        .ap = {.ssid = "dut", .ssid_len = 3, .password = "synthetic",
               .password_len = 9, .security = H2_PAL_WIFI_SECURITY_WPA2, .channel = 6,
               .max_clients = 4},
        .operation_timeout_ms = 50, .client_timeout_ms = 50};
    wifi_test_t state = {.rt = &runtime, .cfg = &config};
    assert(ap_open(&state) == H2_PAL_ERR_UNSUPPORTED);
    assert(fixture.starts == 1 && fixture.stops == 2 && !fixture.active);
    assert(state.result.last_error_line);
    fixture.starts = fixture.stops = 0;
    state.result.last_error_line = 0;
    assert(ap_hidden(&state) == H2_PAL_ERR_UNSUPPORTED);
    assert(fixture.starts == 1 && fixture.stops == 2 && !fixture.active);
    assert(state.result.last_error_line);
}

int main(void) {
    h2_wifi_e2e_config_t config = {.fixture = {.ssid = "fixture", .ssid_len = 7}};
    wifi_test_t good = {.cfg = &config};
    access_point(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED,
                 H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STARTED);
    station(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTING, 0);
    station(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED, 0);
    station(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP, 1);
    station(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED, 0);
    station(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED, 0);
    client(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED, 0, 0);
    client(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED, 1, 0xC0A8BC64u);
    /* ESP reports L2 departure before its cached accepted lease is released. */
    client(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT, 1, 0xC0A8BC64u);
    client(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED, 1, 0xC0A8BC64u);
    access_point(&good, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED,
                 H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STOPPED);
    h2_runtime_system_event_netif_default_changed_t route = {
        .current_valid = 1, .current = {.kind = H2_RUNTIME_SYSTEM_NETIF_KIND_WIFI_STA,
                                        .id_valid = 1, .id = 2}};
    h2_runtime_event_t route_event = {.kind = H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED,
                                      .component = H2_RUNTIME_COMPONENT_SYSTEM_NETIF,
                                      .payload = &route, .payload_size = sizeof(route)};
    observe_event(&good, &route_event);
    assert(good.result.invalid_events == 0 && good.result.sta_lost_ip == 1);
    assert(good.result.lease_granted == 1 && good.result.lease_released == 1);
    assert(good.result.ap_started == 1 && good.result.ap_stopped == 1 &&
           good.result.route_changed == 1);
    assert(good.peers[0].joins == 1 && good.peers[0].lefts == 1 &&
           !good.peers[0].active && !good.peers[0].leased);

    wifi_test_t no_lease = {.cfg = &config};
    client(&no_lease, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED, 0, 0);
    client(&no_lease, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT, 0, 0);
    assert(no_lease.result.invalid_events == 0 && !no_lease.result.lease_granted &&
           !no_lease.result.lease_released);

    wifi_test_t early_grant = {.cfg = &config};
    client(&early_grant, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED, 1, 0xC0A8BC64u);
    assert(early_grant.result.invalid_events > 0);
    wifi_test_t offer_only = {.cfg = &config};
    client(&offer_only, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED, 1, 0xC0A8BC64u);
    assert(offer_only.result.invalid_events > 0);
    wifi_test_t wrong_release = {.cfg = &config};
    client(&wrong_release, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED, 0, 0);
    client(&wrong_release, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED, 1, 0xC0A8BC64u);
    client(&wrong_release, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED, 1, 0xC0A8BC65u);
    assert(wrong_release.result.invalid_events > 0);
    wifi_test_t never_had_ip = {.cfg = &config};
    station(&never_had_ip, H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP,
            H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED, 0);
    assert(never_had_ip.result.invalid_events > 0);
    wifi_test_t bad_ap = {.cfg = &config};
    access_point(&bad_ap, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED,
                 H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STOPPED);
    assert(bad_ap.result.invalid_events > 0);
    test_failed_ap_mode_still_stops_ap();
    test_scan_waits_for_target_without_masking_malformed_records();
    return 0;
}
