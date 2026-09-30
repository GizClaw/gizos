#include "h2_pal_wifi_e2e.h"

#include <string.h>
#include <stdio.h>

typedef struct wifi_peer_observation {
    uint8_t mac[6];
    uint32_t lease_ip4;
    unsigned joins, grants, releases, lefts;
    uint8_t active, leased;
} wifi_peer_observation_t;

typedef struct wifi_test {
    h2_runtime_t *rt;
    const h2_wifi_e2e_config_t *cfg;
    h2_wifi_e2e_result_t result;
    h2_pal_wifi_sta_config_t original, sentinel;
    int original_saved;
    h2_pal_netif_ref_t sta_ref, ap_ref;
    uint8_t sta_mac[6], ap_mac[6], fixture_bssid[6], fixture_channel;
    h2_pal_wifi_ap_client_t client;
    uint64_t overall_start;
    int clock_error;
    unsigned fixture_connecting, fixture_connected, fixture_got_ip;
    uint8_t joined_mac[6], left_mac[6];
    uint8_t sta_ip_held;
    wifi_peer_observation_t peers[H2_PAL_WIFI_AP_MAX_CLIENTS];
    unsigned peer_count;
} wifi_test_t;

#define EXPECT(value)                                                                              \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            s->result.last_error_line = __LINE__;                                                  \
            return H2_PAL_ERR_IO;                                                                  \
        }                                                                                          \
    } while (0)
#define CALL(value)                                                                                \
    do {                                                                                           \
        int call_rc = (value);                                                                     \
        if (call_rc != H2_PAL_OK) {                                                                \
            s->result.last_error_line = __LINE__;                                                  \
            return call_rc;                                                                        \
        }                                                                                          \
    } while (0)

int h2_wifi_config_equal(const h2_pal_wifi_sta_config_t *a, const h2_pal_wifi_sta_config_t *b) {
    return a != NULL && b != NULL && a->ssid_len == b->ssid_len &&
           a->password_len == b->password_len && a->bssid_set == b->bssid_set &&
           a->channel == b->channel && memcmp(a->ssid, b->ssid, a->ssid_len) == 0 &&
           memcmp(a->password, b->password, a->password_len) == 0 &&
           memcmp(a->bssid, b->bssid, sizeof(a->bssid)) == 0;
}

static uint64_t now(wifi_test_t *s) {
    uint64_t value = 0;
    if (h2_pal_time_get_monotonic_ms(s->rt->time, &value) != H2_PAL_OK)
        s->clock_error = 1;
    return value;
}

static int mac_valid(const uint8_t mac[6]) {
    static const uint8_t zero[6] = {0};
    return (mac[0] & 1u) == 0u && memcmp(mac, zero, 6u) != 0;
}

static wifi_peer_observation_t *peer_observation(wifi_test_t *s, const uint8_t mac[6],
                                                  int create) {
    for (unsigned i = 0; i < s->peer_count; ++i)
        if (!memcmp(s->peers[i].mac, mac, 6))
            return &s->peers[i];
    if (!create) return NULL;
    if (s->peer_count == H2_PAL_WIFI_AP_MAX_CLIENTS) {
        ++s->result.invalid_events;
        return NULL;
    }
    wifi_peer_observation_t *peer = &s->peers[s->peer_count++];
    memset(peer, 0, sizeof(*peer));
    memcpy(peer->mac, mac, 6);
    return peer;
}

static void observe_event(wifi_test_t *s, const h2_runtime_event_t *event) {
    const h2_runtime_event_t e = *event;
    h2_wifi_e2e_result_t *r = &s->result;
    switch (e.kind) {
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP: {
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_WIFI ||
                e.payload_size != sizeof(h2_runtime_system_event_wifi_sta_t)) {
                ++r->invalid_events;
                break;
            }
            const h2_runtime_system_event_wifi_sta_t *v = e.payload;
            if (v->ssid_len > H2_PAL_WIFI_SSID_MAX || v->ip_valid > 1u ||
                (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP &&
                 (!v->ip_valid || !v->ip.ip4 ||
                  v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP)))
                ++r->invalid_events;
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING) {
                if (v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTING || v->ip_valid)
                    ++r->invalid_events;
                ++r->sta_connecting;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED) {
                if (v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED || v->ip_valid)
                    ++r->invalid_events;
                ++r->sta_connected;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP) {
                if (!v->bssid_set || !mac_valid(v->bssid) || !v->channel ||
                    v->channel > 14u || !v->ip.netmask4 || !v->ip.gateway4)
                    ++r->invalid_events;
                s->sta_ip_held = 1u;
                ++r->sta_got_ip;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP) {
                if (!s->sta_ip_held || v->ip_valid ||
                    (v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED &&
                     v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED))
                    ++r->invalid_events;
                s->sta_ip_held = 0u;
                ++r->sta_lost_ip;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED) {
                if (s->sta_ip_held || v->ip_valid ||
                    v->status != H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED)
                    ++r->invalid_events;
                s->sta_ip_held = 0u;
                ++r->sta_disconnected;
            }
            if (v->ssid_len == s->cfg->fixture.ssid_len &&
                !memcmp(v->ssid, s->cfg->fixture.ssid, v->ssid_len)) {
                if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING)
                    ++s->fixture_connecting;
                if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED)
                    ++s->fixture_connected;
                if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP)
                    ++s->fixture_got_ip;
            }
            break;
        }
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED: {
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_WIFI ||
                e.payload_size != sizeof(h2_runtime_system_event_wifi_ap_t)) {
                ++r->invalid_events;
                break;
            }
            const h2_runtime_system_event_wifi_ap_t *v = e.payload;
            if (v->ssid_len > H2_PAL_WIFI_SSID_MAX || v->client_count > H2_PAL_WIFI_AP_MAX_CLIENTS)
                ++r->invalid_events;
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED) {
                if (v->status != H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STARTED || !v->ssid_len)
                    ++r->invalid_events;
                ++r->ap_started;
            } else {
                if (v->status != H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STOPPED || v->client_count)
                    ++r->invalid_events;
                ++r->ap_stopped;
            }
            break;
        }
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED:
        case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED: {
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_WIFI ||
                e.payload_size != sizeof(h2_runtime_system_event_wifi_ap_client_t)) {
                ++r->invalid_events;
                break;
            }
            const h2_runtime_system_event_wifi_ap_client_t *v = e.payload;
            if (!mac_valid(v->mac) || v->lease_valid > 1u || (v->lease_valid && !v->lease.ip4)) {
                ++r->invalid_events;
                break;
            }
            wifi_peer_observation_t *peer = peer_observation(
                s, v->mac, e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED);
            if (peer == NULL) {
                ++r->invalid_events;
                break;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED) {
                /* A DHCP OFFER reserves an IP in some SDKs. JOIN must not
                 * advertise that provisional address as an accepted lease. */
                if (peer->active || peer->leased || v->lease_valid)
                    ++r->invalid_events;
                peer->active = 1u;
                peer->lease_ip4 = 0u;
                ++peer->joins;
                ++r->client_joined;
                memcpy(s->joined_mac, v->mac, 6);
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT) {
                if (!peer->active || (v->lease_valid &&
                                      (!peer->leased || v->lease.ip4 != peer->lease_ip4)))
                    ++r->invalid_events;
                peer->active = 0u;
                ++peer->lefts;
                ++r->client_left;
                memcpy(s->left_mac, v->mac, 6);
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED) {
                if (!peer->active || !v->lease_valid ||
                    (peer->leased && peer->lease_ip4 != v->lease.ip4))
                    ++r->invalid_events;
                peer->leased = 1u;
                peer->lease_ip4 = v->lease.ip4;
                ++peer->grants;
                ++r->lease_granted;
            }
            if (e.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED) {
                if (!peer->leased || !v->lease_valid || v->lease.ip4 != peer->lease_ip4)
                    ++r->invalid_events;
                peer->leased = 0u;
                ++peer->releases;
                ++r->lease_released;
            }
            break;
        }
        case H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED: {
            const h2_runtime_system_event_netif_default_changed_t *v = e.payload;
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_NETIF || e.payload_size != sizeof(*v) ||
                v->previous_valid > 1u || v->current_valid > 1u ||
                (v->previous_valid && !v->previous.name_valid && !v->previous.id_valid) ||
                (v->current_valid && !v->current.name_valid && !v->current.id_valid) ||
                (v->previous_valid && v->previous.kind == H2_RUNTIME_SYSTEM_NETIF_KIND_UNKNOWN) ||
                (v->current_valid && v->current.kind == H2_RUNTIME_SYSTEM_NETIF_KIND_UNKNOWN))
                ++r->invalid_events;
            ++r->route_changed;
            break;
        }
        default:
            break;
    }
}

static void events(wifi_test_t *s) {
    union {
        uint64_t alignment;
        unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    } payload;
    h2_runtime_event_t e = {.payload = payload.bytes, .payload_capacity = sizeof(payload.bytes)};
    for (unsigned i = 0; i < 128u && h2_runtime_poll_event(s->rt, &e) == H2_PAL_OK; ++i)
        observe_event(s, &e);
}

static void pause_ms(wifi_test_t *s, uint32_t ms) {
    events(s);
    (void)h2_pal_time_sleep_ms(s->rt->time, ms);
    events(s);
}

static int saved_equal(wifi_test_t *s, const h2_pal_wifi_sta_config_t *config) {
    h2_pal_wifi_sta_config_t got = {0};
    int has = 0;
    CALL(h2_pal_wifi_settings_has_saved_sta_config(s->rt->wifi_settings, &has));
    EXPECT(has == 1);
    CALL(h2_pal_wifi_settings_get_saved_sta_config(s->rt->wifi_settings, &got));
    EXPECT(h2_wifi_config_equal(&got, config));
    return H2_PAL_OK;
}

static int wait_ip(wifi_test_t *s, const h2_pal_wifi_sta_config_t *target) {
    uint64_t start = now(s);
    do {
        h2_pal_wifi_sta_status_t status = {0};
        CALL(h2_pal_wifi_sta_get_status(s->rt->wifi_sta, &status));
        if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid && status.ip.ip4 &&
            status.ssid_len == target->ssid_len &&
            memcmp(status.ssid, target->ssid, target->ssid_len) == 0)
            return H2_PAL_OK;
        pause_ms(s, 100u);
    } while (!s->clock_error && now(s) - start < s->cfg->operation_timeout_ms);
    return H2_PAL_ERR_TIMEOUT;
}

static int connect_target(wifi_test_t *s, const h2_pal_wifi_sta_config_t *target) {
    CALL(h2_pal_wifi_sta_disconnect(s->rt->wifi_sta));
    pause_ms(s, 300u);
    CALL(h2_pal_wifi_sta_connect(s->rt->wifi_sta, target, s->cfg->operation_timeout_ms));
    return wait_ip(s, target);
}

static bool unused_scan(void *user, const h2_pal_wifi_scan_entry_t *entry) {
    (void)user;
    (void)entry;
    return true;
}

static int arguments(wifi_test_t *s) {
    h2_pal_wifi_sta_status_t sta = {0};
    h2_pal_wifi_ap_status_t ap = {0};
    h2_pal_wifi_sta_config_t c = s->cfg->fixture;
    h2_pal_wifi_ap_config_t a = s->cfg->ap;
    h2_pal_netif_status_t n;
    h2_pal_netif_ref_t ref;
    size_t count;
    int has;
    uint8_t mac[6];
    EXPECT(h2_pal_wifi_sta_get_status(NULL, &sta) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_get_status(s->rt->wifi_sta, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_scan(s->rt->wifi_sta, NULL, NULL, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
    h2_pal_wifi_scan_request_t scan = {.ssid_len = H2_PAL_WIFI_SSID_MAX + 1};
    EXPECT(h2_pal_wifi_sta_scan(s->rt->wifi_sta, &scan, unused_scan, NULL, 1) ==
           H2_PAL_ERR_INVALID_ARG);
    c.ssid_len = 0;
    EXPECT(h2_pal_wifi_sta_connect(s->rt->wifi_sta, &c, 1) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_connect_and_save(s->rt->wifi_sta, &c, 1) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_disconnect(NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_get_mac(s->rt->wifi_sta, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_get_mac(NULL, mac) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, (h2_pal_wifi_power_save_t)99) ==
           H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_get_status(NULL, &ap) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_get_status(s->rt->wifi_ap, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_start(s->rt->wifi_ap, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
    a.max_clients = H2_PAL_WIFI_AP_MAX_CLIENTS + 1;
    EXPECT(h2_pal_wifi_ap_start(s->rt->wifi_ap, &a, 1) == H2_PAL_ERR_INVALID_ARG);
    a = s->cfg->ap;
    a.password_len = 7;
    EXPECT(h2_pal_wifi_ap_start(s->rt->wifi_ap, &a, 1) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_stop(NULL, 1) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_get_mac(s->rt->wifi_ap, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, NULL, 1, &count) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, NULL, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_settings_get_saved_sta_config(s->rt->wifi_settings, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_settings_clear_saved_sta_config(NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_wifi_settings_has_saved_sta_config(NULL, &has) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_netif_list(s->rt->netif, NULL, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_netif_find(s->rt->netif, NULL, &ref) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_netif_get_status(s->rt->netif, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    EXPECT(h2_pal_netif_get_dns_servers(s->rt->netif, NULL, NULL, 1, &count) ==
           H2_PAL_ERR_INVALID_ARG);
    ref = h2_pal_netif_default_ref();
    EXPECT(h2_pal_netif_set_default(s->rt->netif, &ref) == H2_PAL_ERR_INVALID_ARG);
    (void)n;
    return H2_PAL_OK;
}

static int station_mac(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_get_mac(s->rt->wifi_sta, s->sta_mac));
    EXPECT(mac_valid(s->sta_mac));
    return H2_PAL_OK;
}
static int access_point_mac(wifi_test_t *s) {
    CALL(h2_pal_wifi_ap_get_mac(s->rt->wifi_ap, s->ap_mac));
    EXPECT(mac_valid(s->ap_mac));
    return H2_PAL_OK;
}
static int settings_clear(wifi_test_t *s) {
    CALL(h2_pal_wifi_settings_clear_saved_sta_config(s->rt->wifi_settings));
    int has = 1;
    h2_pal_wifi_sta_config_t got = {0};
    CALL(h2_pal_wifi_settings_has_saved_sta_config(s->rt->wifi_settings, &has));
    EXPECT(has == 0);
    EXPECT(h2_pal_wifi_settings_get_saved_sta_config(s->rt->wifi_settings, &got) ==
           H2_PAL_ERR_NOT_FOUND);
    CALL(h2_pal_wifi_settings_clear_saved_sta_config(s->rt->wifi_settings));
    return H2_PAL_OK;
}
static int settings_boundaries(wifi_test_t *s) {
    memset(&s->sentinel, 0, sizeof(s->sentinel));
    memset(s->sentinel.ssid, 's', H2_PAL_WIFI_SSID_MAX);
    memset(s->sentinel.password, 'a', H2_PAL_WIFI_PASSWORD_MAX);
    s->sentinel.ssid_len = H2_PAL_WIFI_SSID_MAX;
    s->sentinel.password_len = H2_PAL_WIFI_PASSWORD_MAX;
    s->sentinel.bssid_set = 1;
    s->sentinel.channel = 6;
    s->sentinel.bssid[0] = 2;
    s->sentinel.bssid[5] = 7;
    CALL(h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, &s->sentinel));
    return saved_equal(s, &s->sentinel);
}
static int settings_invalid(wifi_test_t *s) {
    h2_pal_wifi_sta_config_t c = s->sentinel;
    c.ssid_len = 33;
    EXPECT(h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, &c) ==
           H2_PAL_ERR_INVALID_ARG);
    c = s->sentinel;
    c.password_len = 65;
    EXPECT(h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, &c) ==
           H2_PAL_ERR_INVALID_ARG);
    c = s->sentinel;
    c.ssid_len = 0;
    EXPECT(h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, &c) ==
           H2_PAL_ERR_INVALID_ARG);
    return saved_equal(s, &s->sentinel);
}

typedef struct scan_observation {
    const h2_pal_wifi_sta_config_t *target;
    unsigned count, target_count;
    int bad, early;
    uint8_t channel, bssid[6];
} scan_observation_t;
static bool scan_result(void *user, const h2_pal_wifi_scan_entry_t *e) {
    scan_observation_t *o = user;
    ++o->count;
    if (e == NULL || e->ssid_len > 32 || !mac_valid(e->bssid) || e->channel == 0 || e->rssi > 0)
        o->bad = 1;
    if (e != NULL && e->ssid_len == o->target->ssid_len &&
        memcmp(e->ssid, o->target->ssid, e->ssid_len) == 0) {
        ++o->target_count;
        o->channel = e->channel;
        memcpy(o->bssid, e->bssid, 6);
    }
    return !o->early;
}
static int scan_test(wifi_test_t *s, int directed, int early) {
    scan_observation_t o = {.target = &s->cfg->fixture, .early = early};
    h2_pal_wifi_scan_request_t request = {0};
    if (directed) {
        request.ssid_len = o.target->ssid_len;
        memcpy(request.ssid, o.target->ssid, request.ssid_len);
        request.channel = o.target->channel;
    }
    /* The independent fixture may still be starting, or may miss one beacon
     * window while acting as a STA itself. Retry only a missing target within
     * a finite budget; malformed records and callback/filter violations still
     * fail. Early-stop only qualifies callback polarity, not target discovery. */
    uint64_t started = now(s);
    for (;;) {
        o = (scan_observation_t){.target = &s->cfg->fixture, .early = early};
        CALL(h2_pal_wifi_sta_scan(s->rt->wifi_sta, directed ? &request : NULL, scan_result, &o,
                                  s->cfg->operation_timeout_ms));
        if (o.bad || (early ? o.count : o.target_count) || s->clock_error ||
            now(s) - started >= s->cfg->operation_timeout_ms)
            break;
        pause_ms(s, 500);
    }
    EXPECT(!o.bad && o.count > 0);
    if (early)
        EXPECT(o.count == 1);
    else
        EXPECT(o.target_count > 0);
    if (directed)
        EXPECT(o.count == o.target_count && (request.channel == 0 || request.channel == o.channel));
    if (o.target_count) {
        memcpy(s->fixture_bssid, o.bssid, 6);
        s->fixture_channel = o.channel;
    }
    return H2_PAL_OK;
}
static int scan_all(wifi_test_t *s) {
    return scan_test(s, 0, 0);
}
static int scan_directed(wifi_test_t *s) {
    return scan_test(s, 1, 0);
}
static int scan_early_stop(wifi_test_t *s) {
    return scan_test(s, 0, 1);
}
static int connect_without_save(wifi_test_t *s) {
    events(s);
    s->fixture_connecting = s->fixture_connected = s->fixture_got_ip = 0;
    CALL(connect_target(s, &s->cfg->fixture));
    return saved_equal(s, &s->sentinel);
}
static int station_events(wifi_test_t *s) {
    pause_ms(s, 300);
    EXPECT(s->fixture_connecting && s->fixture_connected && s->fixture_got_ip);
    return H2_PAL_OK;
}

typedef struct netif_list {
    unsigned count;
    int bad, stop;
    h2_pal_netif_kind_t kind;
    h2_pal_netif_ref_t first;
} netif_list_t;
static int list_netif(void *user, const h2_pal_netif_ref_t *ref,
                      const h2_pal_netif_status_t *status) {
    netif_list_t *o = user;
    ++o->count;
    if (!ref || !status || !h2_pal_netif_ref_is_concrete(ref) ||
        !h2_pal_netif_ref_equal(ref, &status->ref) || (o->kind && status->kind != o->kind))
        o->bad = 1;
    if (o->count == 1 && ref)
        o->first = *ref;
    return o->stop;
}
static int netif_find(wifi_test_t *s) {
    netif_list_t o = {0};
    CALL(h2_pal_netif_list(s->rt->netif, NULL, list_netif, &o));
    EXPECT(o.count && !o.bad);
    h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_STA};
    o = (netif_list_t){.kind = filter.kind};
    CALL(h2_pal_netif_list(s->rt->netif, &filter, list_netif, &o));
    EXPECT(o.count == 1 && !o.bad);
    CALL(h2_pal_netif_find(s->rt->netif, &filter, &s->sta_ref));
    EXPECT(h2_pal_netif_ref_equal(&s->sta_ref, &o.first));
    o = (netif_list_t){.stop = 1};
    EXPECT(h2_pal_netif_list(s->rt->netif, NULL, list_netif, &o) == H2_PAL_EXIT);
    EXPECT(o.count == 1);
    return H2_PAL_OK;
}
static int netif_status(wifi_test_t *s) {
    h2_pal_netif_status_t n = {0};
    h2_pal_wifi_sta_status_t w = {0};
    uint8_t ip[4];
    CALL(h2_pal_netif_get_status(s->rt->netif, &s->sta_ref, &n));
    CALL(h2_pal_wifi_sta_get_status(s->rt->wifi_sta, &w));
    EXPECT(h2_pal_netif_status_is_usable(&n) && w.ip_valid && n.kind == H2_PAL_NETIF_KIND_WIFI_STA);
    EXPECT(n.mtu >= 576 && n.mac_valid && memcmp(n.mac, s->sta_mac, 6) == 0);
    EXPECT(w.bssid_set && mac_valid(w.bssid) && w.channel >= 1 && w.channel <= 14);
    if (w.ssid_len == s->cfg->fixture.ssid_len &&
        !memcmp(w.ssid, s->cfg->fixture.ssid, w.ssid_len)) {
        EXPECT(!memcmp(w.bssid, s->fixture_bssid, 6) && w.channel == s->fixture_channel);
    }
    h2_pal_wifi_ip4_to_bytes(w.ip.ip4, ip);
    EXPECT(n.ipv4.family == H2_PAL_NET_FAMILY_IPV4 && memcmp(n.ipv4.ip, ip, 4) == 0);
    h2_pal_wifi_ip4_to_bytes(w.ip.netmask4, ip);
    EXPECT(memcmp(n.netmask4.ip, ip, 4) == 0);
    h2_pal_wifi_ip4_to_bytes(w.ip.gateway4, ip);
    EXPECT(memcmp(n.gateway4.ip, ip, 4) == 0);
    return H2_PAL_OK;
}
static int netif_dns(wifi_test_t *s) {
    h2_pal_netif_status_t n = {0};
    CALL(h2_pal_netif_get_status(s->rt->netif, &s->sta_ref, &n));
    struct {
        h2_pal_netif_dns_server_t dns[2];
        uint32_t guard;
    } b = {.guard = 0x1234abcd};
    size_t count = 99;
    CALL(h2_pal_netif_get_dns_servers(s->rt->netif, &s->sta_ref, b.dns, 2, &count));
    EXPECT(count == n.dns_count && count <= 2 && b.guard == 0x1234abcd);
    for (size_t i = 0; i < count; ++i)
        EXPECT(memcmp(&b.dns[i].addr, &n.dns[i].addr, sizeof(b.dns[i].addr)) == 0);
    CALL(h2_pal_netif_get_dns_servers(s->rt->netif, &s->sta_ref, NULL, 0, &count));
    EXPECT(count == 0);
    return H2_PAL_OK;
}
static int netif_missing(wifi_test_t *s) {
    h2_pal_netif_ref_t ref = {.type = H2_PAL_NETIF_REF_ID, .id = 0xfffffffeu};
    h2_pal_netif_status_t n;
    h2_pal_netif_filter_t filter = {.id = ref.id};
    size_t count;
    EXPECT(h2_pal_netif_get_status(s->rt->netif, &ref, &n) == H2_PAL_ERR_NOT_FOUND);
    EXPECT(h2_pal_netif_find(s->rt->netif, &filter, &ref) == H2_PAL_ERR_NOT_FOUND);
    EXPECT(h2_pal_netif_get_dns_servers(s->rt->netif, &ref, NULL, 0, &count) ==
           H2_PAL_ERR_NOT_FOUND);
    EXPECT(h2_pal_netif_set_default(s->rt->netif, &ref) == H2_PAL_ERR_NOT_FOUND);
    netif_list_t o = {0};
    CALL(h2_pal_netif_list(s->rt->netif, &filter, list_netif, &o));
    EXPECT(o.count == 0);
    return H2_PAL_OK;
}
static int scan_connected(wifi_test_t *s) {
    CALL(scan_test(s, 1, 0));
    return netif_status(s);
}
static int power_none(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_NONE));
    return wait_ip(s, &s->cfg->fixture);
}
static int power_min(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_MIN_MODEM));
    return wait_ip(s, &s->cfg->fixture);
}
static int power_max(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_MAX_MODEM));
    CALL(wait_ip(s, &s->cfg->fixture));
    /* Restoring MIN after MAX must select the smaller policy, then leave
     * MAX selected so later real reconnects exercise its future-link policy. */
    CALL(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_MIN_MODEM));
    CALL(wait_ip(s, &s->cfg->fixture));
    CALL(h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_MAX_MODEM));
    return wait_ip(s, &s->cfg->fixture);
}
static int save_zero(wifi_test_t *s) {
    EXPECT(h2_pal_wifi_sta_connect_and_save(s->rt->wifi_sta, &s->cfg->fixture, 0) ==
           H2_PAL_ERR_INVALID_ARG);
    CALL(wait_ip(s, &s->cfg->fixture));
    return saved_equal(s, &s->sentinel);
}
static int wrong_password(wifi_test_t *s) {
    h2_pal_wifi_sta_config_t bad = s->cfg->fixture;
    bad.password[0] = bad.password[0] == 'x' ? 'y' : 'x';
    int rc = h2_pal_wifi_sta_connect_and_save(s->rt->wifi_sta, &bad, 8000);
    EXPECT(rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_INVALID_STATE);
    CALL(saved_equal(s, &s->sentinel));
    /* Reconnect correctly to the same peer: a missing AP cannot qualify this
     * credential rejection, and a stale cached association cannot validate it. */
    CALL(connect_target(s, &s->cfg->fixture));
    return H2_PAL_OK;
}
static int connect_save(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_connect_and_save(s->rt->wifi_sta, &s->cfg->fixture,
                                          s->cfg->operation_timeout_ms));
    CALL(wait_ip(s, &s->cfg->fixture));
    return saved_equal(s, &s->cfg->fixture);
}
static int disconnect_status(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_disconnect(s->rt->wifi_sta));
    pause_ms(s, 500);
    h2_pal_wifi_sta_status_t w = {0};
    h2_pal_netif_status_t n = {0};
    CALL(h2_pal_wifi_sta_get_status(s->rt->wifi_sta, &w));
    EXPECT(!w.ip_valid && !w.ip.ip4);
    EXPECT(w.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED || w.state == H2_PAL_WIFI_STA_STATE_IDLE);
    const h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_STA};
    CALL(h2_pal_netif_find(s->rt->netif, &filter, &s->sta_ref));
    CALL(h2_pal_netif_get_status(s->rt->netif, &s->sta_ref, &n));
    EXPECT((n.flags & (H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_HAS_IPV4 |
                       H2_PAL_NETIF_FLAG_DEFAULT_ROUTE)) == 0);
    return saved_equal(s, &s->cfg->fixture);
}
static int connect_zero(wifi_test_t *s) {
    h2_pal_wifi_sta_config_t borrowed = s->cfg->fixture;
    CALL(h2_pal_wifi_sta_connect(s->rt->wifi_sta, &borrowed, 0));
    memset(&borrowed, 0xa5, sizeof(borrowed));
    CALL(wait_ip(s, &s->cfg->fixture));
    return saved_equal(s, &s->cfg->fixture);
}
static int ap_start(wifi_test_t *s) {
    CALL(h2_pal_wifi_sta_disconnect(s->rt->wifi_sta));
    pause_ms(s, 300);
    CALL(h2_pal_wifi_ap_start(s->rt->wifi_ap, &s->cfg->ap, s->cfg->operation_timeout_ms));
    pause_ms(s, 300);
    return H2_PAL_OK;
}
static int ap_status(wifi_test_t *s) {
    h2_pal_wifi_ap_status_t a = {0};
    CALL(h2_pal_wifi_ap_get_status(s->rt->wifi_ap, &a));
    EXPECT(a.state == H2_PAL_WIFI_AP_STATE_STARTED && a.ssid_len == s->cfg->ap.ssid_len);
    EXPECT(memcmp(a.ssid, s->cfg->ap.ssid, a.ssid_len) == 0 &&
           a.security == H2_PAL_WIFI_SECURITY_WPA2 && a.channel == 6);
    EXPECT(a.max_clients == s->cfg->ap.max_clients && a.hidden == 0);
    return H2_PAL_OK;
}
static int ap_netif(wifi_test_t *s) {
    h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_AP};
    h2_pal_netif_status_t n = {0};
    CALL(h2_pal_netif_find(s->rt->netif, &filter, &s->ap_ref));
    CALL(h2_pal_netif_get_status(s->rt->netif, &s->ap_ref, &n));
    EXPECT(n.kind == H2_PAL_NETIF_KIND_WIFI_AP && n.mtu >= 576 && n.mac_valid &&
           memcmp(n.mac, s->ap_mac, 6) == 0);
    EXPECT((n.flags & H2_PAL_NETIF_FLAG_HAS_IPV4) && n.ipv4.family == H2_PAL_NET_FAMILY_IPV4);
    return H2_PAL_OK;
}
static int ap_client(wifi_test_t *s) {
    uint64_t start = now(s);
    do {
        h2_pal_wifi_ap_client_t clients[8] = {0};
        size_t count = 0;
        CALL(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, clients, 8, &count));
        EXPECT(count <= 8);
        if (count && clients[0].lease_valid && clients[0].lease.ip4) {
            EXPECT(mac_valid(clients[0].mac));
            events(s);
            wifi_peer_observation_t *peer = peer_observation(s, clients[0].mac, 0);
            /* Some SDKs expose the IP reserved at OFFER time in get_clients.
             * Only an actual DHCP ACK/ASSIGNED event establishes this lease. */
            if (peer == NULL || !peer->active || !peer->leased ||
                peer->lease_ip4 != clients[0].lease.ip4) {
                pause_ms(s, 100);
                continue;
            }
            h2_pal_netif_status_t status = {0};
            CALL(h2_pal_netif_get_status(s->rt->netif, &s->ap_ref, &status));
            const uint8_t *a = status.ipv4.ip, *m = status.netmask4.ip;
            uint32_t address =
                (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
            uint32_t mask =
                (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
            char observation[160];
            (void)snprintf(observation, sizeof(observation),
                           "H2_WIFI_ADDRESS lease=%lu ap=%lu mask=%lu",
                           (unsigned long)clients[0].lease.ip4, (unsigned long)address,
                           (unsigned long)mask);
            (void)h2_pal_log_write(s->rt->log, H2_PAL_LOG_INFO, "pal-wifi", observation);
            EXPECT(mask && (clients[0].lease.ip4 & mask) == (address & mask));
            s->client = clients[0];
            memcpy(s->result.client_mac, clients[0].mac, 6);
            s->result.client_ip4 = clients[0].lease.ip4;
            EXPECT(s->result.client_joined > 0 && !memcmp(s->joined_mac, clients[0].mac, 6));
            return H2_PAL_OK;
        }
        pause_ms(s, 100);
    } while (!s->clock_error && now(s) - start < s->cfg->client_timeout_ms);
    return H2_PAL_ERR_TIMEOUT;
}
static int ap_client_bounds(wifi_test_t *s) {
    struct {
        h2_pal_wifi_ap_client_t client;
        uint32_t guard;
    } b = {.guard = 0xabcdef12};
    size_t count = 99;
    CALL(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, &b.client, 1, &count));
    EXPECT(count == 1 && b.guard == 0xabcdef12);
    EXPECT(memcmp(b.client.mac, s->client.mac, 6) == 0);
    CALL(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, NULL, 0, &count));
    EXPECT(count == 0);
    return H2_PAL_OK;
}
static int ap_client_left(wifi_test_t *s) {
    uint64_t start = now(s);
    do {
        h2_pal_wifi_ap_client_t c[8];
        size_t count = 99;
        events(s);
        CALL(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, c, 8, &count));
        wifi_peer_observation_t *peer = peer_observation(s, s->client.mac, 0);
        if (!count && peer != NULL && !peer->active && !peer->leased &&
            peer->lefts && peer->releases && peer->lease_ip4 == s->client.lease.ip4 &&
            !memcmp(s->left_mac, s->client.mac, 6)) {
            const unsigned grants = peer->grants, releases = peer->releases;
            const uint64_t event_only_start = now(s);
            /* The fixture returns after a bounded cooldown. From here until
             * its next accepted lease is released, consume only Runtime
             * events: no AP get_clients/status call may drive reconciliation.
             * This proves autonomous delivery to an event-only consumer. */
            do {
                events(s);
                peer = peer_observation(s, s->client.mac, 0);
                if (peer != NULL && peer->grants > grants && peer->releases > releases &&
                    !peer->active && !peer->leased &&
                    peer->lease_ip4 == s->client.lease.ip4)
                    return H2_PAL_OK;
                pause_ms(s, 100);
            } while (!s->clock_error && now(s) - event_only_start <
                                         2ull * s->cfg->client_timeout_ms);
            return H2_PAL_ERR_TIMEOUT;
        }
        pause_ms(s, 100);
    } while (!s->clock_error && now(s) - start < s->cfg->client_timeout_ms);
    return H2_PAL_ERR_TIMEOUT;
}
static int netif_select(wifi_test_t *s) {
    /* Use the independent infrastructure AP when saved credentials exist.
     * The fixture STA remains a real client of the DUT AP; reciprocal peer
     * AP/STA associations are not part of this interface-routing contract. */
    const h2_pal_wifi_sta_config_t *upstream =
        s->original_saved ? &s->original : &s->cfg->fixture;
    CALL(connect_target(s, upstream));
    const h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_STA};
    CALL(h2_pal_netif_find(s->rt->netif, &filter, &s->sta_ref));
    CALL(h2_pal_netif_set_default(s->rt->netif, &s->ap_ref));
    pause_ms(s, 300);
    h2_pal_netif_status_t n;
    CALL(h2_pal_netif_get_status(s->rt->netif, NULL, &n));
    EXPECT(h2_pal_netif_ref_equal(&n.ref, &s->ap_ref));
    CALL(h2_pal_netif_set_default(s->rt->netif, &s->sta_ref));
    pause_ms(s, 300);
    CALL(h2_pal_netif_get_status(s->rt->netif, NULL, &n));
    EXPECT(h2_pal_netif_ref_equal(&n.ref, &s->sta_ref));
    unsigned before = s->result.route_changed;
    CALL(h2_pal_netif_set_default(s->rt->netif, &s->sta_ref));
    pause_ms(s, 300);
    EXPECT(s->result.route_changed == before);
    return netif_dns(s);
}
static int ap_repeat(wifi_test_t *s) {
    events(s);
    unsigned before = s->result.ap_started;
    CALL(h2_pal_wifi_ap_start(s->rt->wifi_ap, &s->cfg->ap, s->cfg->operation_timeout_ms));
    pause_ms(s, 300);
    EXPECT(s->result.ap_started == before);
    return ap_status(s);
}
static int ap_stop(wifi_test_t *s) {
    CALL(h2_pal_wifi_ap_stop(s->rt->wifi_ap, s->cfg->operation_timeout_ms));
    pause_ms(s, 300);
    h2_pal_wifi_ap_status_t a;
    CALL(h2_pal_wifi_ap_get_status(s->rt->wifi_ap, &a));
    EXPECT(a.state == H2_PAL_WIFI_AP_STATE_STOPPED && a.client_count == 0);
    size_t count;
    CALL(h2_pal_wifi_ap_get_clients(s->rt->wifi_ap, NULL, 0, &count));
    EXPECT(count == 0);
    CALL(h2_pal_wifi_ap_stop(s->rt->wifi_ap, s->cfg->operation_timeout_ms));
    return H2_PAL_OK;
}
static int ap_open_checks(wifi_test_t *s) {
    /* Coexistence was tested with the independent upstream already. Stop its
     * STA before standalone AP modes so their requested channel is stable. */
    CALL(h2_pal_wifi_sta_disconnect(s->rt->wifi_sta));
    pause_ms(s, 300);
    h2_pal_wifi_ap_config_t c = s->cfg->ap;
    c.security = H2_PAL_WIFI_SECURITY_OPEN;
    c.password_len = 0;
    memset(c.password, 0, sizeof(c.password));
    CALL(h2_pal_wifi_ap_start(s->rt->wifi_ap, &c, s->cfg->operation_timeout_ms));
    pause_ms(s, 300);
    h2_pal_wifi_ap_status_t status;
    CALL(h2_pal_wifi_ap_get_status(s->rt->wifi_ap, &status));
    EXPECT(status.security == H2_PAL_WIFI_SECURITY_OPEN);
    CALL(ap_netif(s));
    CALL(ap_client(s));
    CALL(ap_client_left(s));
    return H2_PAL_OK;
}
static int ap_open(wifi_test_t *s) {
    int rc = ap_open_checks(s);
    unsigned first_line = s->result.last_error_line;
    int stop_rc = ap_stop(s);
    if (rc != H2_PAL_OK) {
        s->result.last_error_line = first_line;
        return rc;
    }
    return stop_rc;
}
static int ap_hidden_checks(wifi_test_t *s) {
    h2_pal_wifi_ap_config_t c = s->cfg->ap;
    c.hidden = 1;
    CALL(h2_pal_wifi_ap_start(s->rt->wifi_ap, &c, s->cfg->operation_timeout_ms));
    pause_ms(s, 300);
    h2_pal_wifi_ap_status_t status;
    CALL(h2_pal_wifi_ap_get_status(s->rt->wifi_ap, &status));
    EXPECT(status.hidden == 1);
    CALL(ap_netif(s));
    CALL(ap_client(s));
    CALL(ap_client_left(s));
    return H2_PAL_OK;
}
static int ap_hidden(wifi_test_t *s) {
    int rc = ap_hidden_checks(s);
    unsigned first_line = s->result.last_error_line;
    int stop_rc = ap_stop(s);
    if (rc != H2_PAL_OK) {
        s->result.last_error_line = first_line;
        return rc;
    }
    return stop_rc;
}
static int churn(wifi_test_t *s) {
    for (unsigned i = 0; i < 2; ++i) {
        CALL(connect_target(s, &s->cfg->fixture));
        CALL(h2_pal_wifi_sta_disconnect(s->rt->wifi_sta));
        pause_ms(s, 300);
        CALL(h2_pal_wifi_ap_start(s->rt->wifi_ap, &s->cfg->ap, s->cfg->operation_timeout_ms));
        pause_ms(s, 300);
        CALL(ap_stop(s));
    }
    return saved_equal(s, &s->cfg->fixture);
}
static int event_integrity(wifi_test_t *s) {
    events(s);
    EXPECT(!s->result.invalid_events);
    EXPECT(s->result.sta_connecting && s->result.sta_connected && s->result.sta_got_ip &&
           s->result.sta_lost_ip && s->result.sta_disconnected);
    EXPECT(s->result.ap_started && s->result.ap_stopped && s->result.client_joined &&
           s->result.client_left && s->result.route_changed);
    EXPECT(s->result.lease_granted >= 3u && s->result.lease_released >= 3u);
    wifi_peer_observation_t *peer = peer_observation(s, s->result.client_mac, 0);
    EXPECT(peer != NULL && peer->joins >= 3u && peer->grants >= 3u &&
           peer->lefts >= 3u && peer->releases >= 3u && !peer->active && !peer->leased &&
           peer->lease_ip4 == s->result.client_ip4);
    return H2_PAL_OK;
}

static int sta_is_disconnected(const h2_pal_wifi_sta_status_t *status) {
    return !status->ip_valid &&
           (status->state == H2_PAL_WIFI_STA_STATE_IDLE ||
            status->state == H2_PAL_WIFI_STA_STATE_DISCONNECTED ||
            status->state == H2_PAL_WIFI_STA_STATE_FAILED);
}

static int restore(wifi_test_t *s) {
    /* Aggregate every restoration error, including after earlier failures.
     * Never return early and strand temporary AP or original credentials. */
    int rc = h2_pal_wifi_ap_stop(s->rt->wifi_ap, s->cfg->operation_timeout_ms);
    int next = h2_pal_wifi_sta_disconnect(s->rt->wifi_sta);
    if (!rc)
        rc = next;
    next = s->original_saved
               ? h2_pal_wifi_settings_set_saved_sta_config(s->rt->wifi_settings, &s->original)
               : h2_pal_wifi_settings_clear_saved_sta_config(s->rt->wifi_settings);
    if (!rc)
        rc = next;
    if (!next) {
        if (s->original_saved)
            next = saved_equal(s, &s->original);
        else {
            int has = 1;
            next = h2_pal_wifi_settings_has_saved_sta_config(s->rt->wifi_settings, &has);
            if (!next && has)
                next = H2_PAL_ERR_IO;
        }
        if (!rc)
            rc = next;
        s->result.saved_restored = !next;
    }
    next = h2_pal_wifi_sta_set_power_save(s->rt->wifi_sta, H2_PAL_WIFI_POWER_SAVE_NONE);
    if (!rc)
        rc = next;
    if (s->original_saved) {
        next = connect_target(s, &s->original);
        if (!rc)
            rc = next;
        s->result.network_restored = !next;
    } else {
        h2_pal_wifi_sta_status_t status = {0};
        next = h2_pal_wifi_sta_get_status(s->rt->wifi_sta, &status);
        if (!next && !sta_is_disconnected(&status))
            next = H2_PAL_ERR_IO;
        if (!rc)
            rc = next;
        s->result.network_restored = !next;
    }
    s->result.cleanup_rc = rc;
    s->result.retained = rc ? 1u : 0u;
    return rc;
}

typedef struct wifi_case {
    const char *id;
    int (*run)(wifi_test_t *);
} wifi_case_t;
#define H2_WIFI_CASE(id, function) {id, function},
static const wifi_case_t cases[] = {
#include "h2_pal_wifi_cases.inc"
};
#undef H2_WIFI_CASE

int h2_wifi_e2e_run(h2_runtime_t *rt, const h2_wifi_e2e_config_t *cfg, h2_wifi_e2e_result_t *out) {
    if (!rt || !cfg || !out || !cfg->operation_timeout_ms || !cfg->client_timeout_ms ||
        h2_pal_wifi_settings_validate_sta_config(&cfg->fixture) ||
        h2_pal_wifi_ap_config_validate(&cfg->ap))
        return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    wifi_test_t s = {.rt = rt, .cfg = cfg};
    int rc = h2_pal_wifi_settings_has_saved_sta_config(rt->wifi_settings, &s.original_saved);
    if (!rc && s.original_saved)
        rc = h2_pal_wifi_settings_get_saved_sta_config(rt->wifi_settings, &s.original);
    if (!rc && !s.original_saved) {
        /* Status cannot recover the password of a borrowed connection. Only
         * begin from a known disconnected state when no configuration can
         * restore it; reject before any radio or Settings mutation. */
        h2_pal_wifi_sta_status_t status = {0};
        rc = h2_pal_wifi_sta_get_status(rt->wifi_sta, &status);
        if (!rc && !sta_is_disconnected(&status))
            rc = H2_PAL_ERR_BUSY;
    }
    h2_pal_wifi_ap_status_t ap = {0};
    if (!rc)
        rc = h2_pal_wifi_ap_get_status(rt->wifi_ap, &ap);
    if (!rc && ap.state != H2_PAL_WIFI_AP_STATE_STOPPED)
        rc = H2_PAL_ERR_BUSY;
    if (rc) {
        out->blocked = (unsigned)(sizeof(cases) / sizeof(cases[0]));
        out->retained = 0;
        return rc;
    }
    s.overall_start = now(&s);
    if (s.clock_error) {
        out->blocked = (unsigned)(sizeof(cases) / sizeof(cases[0]));
        return H2_PAL_ERR_UNAVAILABLE;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t start = now(&s);
        s.result.last_error_line = 0;
        rc = (s.clock_error || now(&s) - s.overall_start > 600000u) && cases[i].run != restore
                 ? H2_PAL_ERR_TIMEOUT
                 : cases[i].run(&s);
        events(&s);
        /* A restoration transition can itself publish a malformed event.
         * Include that last drain in the mandatory verdict before confirming. */
        if (!rc && cases[i].run == restore && s.result.invalid_events) {
            s.result.last_error_line = __LINE__;
            rc = H2_PAL_ERR_IO;
        }
        if (!rc)
            ++s.result.passed;
        else if (rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_UNAVAILABLE)
            ++s.result.blocked;
        else
            ++s.result.failed;
        *out = s.result;
        if (cfg->case_result)
            cfg->case_result(cfg->user, cases[i].id, rc, now(&s) - start);
    }
    /* Password-bearing automatic storage is wiped before return. No report
     * callback receives it; crash recovery belongs to the durable launcher. */
    memset(&s.original, 0, sizeof(s.original));
    memset(&s.sentinel, 0, sizeof(s.sentinel));
    return out->failed || out->blocked || out->cleanup_rc || out->retained ? H2_PAL_ERR_IO
                                                                           : H2_PAL_OK;
}
