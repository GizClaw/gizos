#include "h2_wifi_sta.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    h2_pal_wifi_sta_config_t saved, target;
    h2_pal_wifi_sta_status_t status;
    uint64_t now;
    uint32_t connect_ms, disconnect_ms, timeout;
    unsigned int connects, disconnects, saves, reads, ready_after;
    int connect_rc, save_rc, status_rc, sleep_rc, get_rc;
    bool wrong_ssid, wrong_bssid, zero_ip;
} fixture_t;

static int get_saved(void *user, h2_pal_wifi_sta_config_t *out) {
    fixture_t *f = user;
    *out = f->saved;
    return f->get_rc ? f->get_rc : (out->ssid_len ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND);
}
static int save(void *user, const h2_pal_wifi_sta_config_t *config) {
    fixture_t *f = user;
    assert(f->status.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
    assert(f->status.ip_valid && f->status.ip.ip4);
    assert(!f->wrong_ssid && !f->wrong_bssid);
    ++f->saves;
    if (!f->save_rc) f->saved = *config;
    return f->save_rc;
}
static int connect(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout) {
    fixture_t *f = user;
    ++f->connects;
    f->target = *config;
    f->timeout = timeout;
    f->now += f->connect_ms;
    f->reads = 0;
    f->status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_CONNECTING};
    return f->connect_rc;
}
static int disconnect(void *user) {
    fixture_t *f = user;
    ++f->disconnects;
    f->now += f->disconnect_ms;
    f->status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_IDLE};
    return H2_PAL_OK;
}
static int status(void *user, h2_pal_wifi_sta_status_t *out) {
    fixture_t *f = user;
    if (f->connects && ++f->reads > f->ready_after) {
        f->status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
        f->status.ip_valid = 1;
        f->status.ip.ip4 = f->zero_ip ? 0 : 0xc0000201;
        f->status.ssid_len = f->target.ssid_len;
        memcpy(f->status.ssid, f->target.ssid, f->target.ssid_len);
        if (f->wrong_ssid) f->status.ssid[0] ^= 1;
        f->status.bssid_set = f->target.bssid_set;
        memcpy(f->status.bssid, f->target.bssid, 6);
        if (f->wrong_bssid) f->status.bssid[0] ^= 1;
    }
    *out = f->status;
    return f->status_rc;
}
static h2_pal_result_t now(void *user, uint64_t *out) {
    *out = ((fixture_t *)user)->now;
    return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
    fixture_t *f = user;
    f->now += ms;
    return f->sleep_rc;
}
static const h2_pal_wifi_sta_vtable_t sta_vtable = {
    .connect = connect, .disconnect = disconnect, .get_status = status};
static const h2_pal_wifi_settings_vtable_t settings_vtable = {
    .get_saved_sta_config = get_saved, .set_saved_sta_config = save};
static const h2_pal_time_vtable_t time_vtable = {
    .get_monotonic_ms = now, .sleep_ms = sleep_ms};

static void run_cases(void) {
    fixture_t f = {0};
    const h2_pal_wifi_sta_api_t sta = {&f, &sta_vtable};
    const h2_pal_wifi_settings_api_t settings = {&f, &settings_vtable};
    const h2_pal_time_api_t time = {&f, &time_vtable};
    h2_wifi_sta_dependencies_t deps = {&sta, &settings, &time};
    const h2_pal_wifi_sta_config_t old = {.ssid = "home", .ssid_len = 4,
        .password = "old-password", .password_len = 12};
    h2_pal_wifi_sta_config_t target = {.ssid = "temporary", .ssid_len = 9,
        .password = "new-password", .password_len = 12};
    const uint32_t budgets[] = {0, 1, 100, UINT32_MAX};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        f = (fixture_t){0};
        assert(h2_pal_wifi_sta_connect(&sta, &target, budgets[i]) == H2_PAL_OK);
        assert(!f.saves && !f.saved.ssid_len);
        f.saved = old;
        assert(h2_pal_wifi_sta_connect(&sta, &target, budgets[i]) == H2_PAL_OK);
        assert(!f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    }
    f = (fixture_t){.saved = old, .ready_after = 2};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_OK);
    assert(f.saves == 1 && f.reads == 3 && f.now == 40);
    assert(!memcmp(&f.saved, &target, sizeof(target)));
    f = (fixture_t){.saved = old, .connect_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    f = (fixture_t){.saved = old, .save_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(f.saves == 1 && !memcmp(&f.saved, &old, sizeof(old)));
    for (unsigned int i = 0; i < 4; ++i) {
        f = (fixture_t){.saved = old, .wrong_ssid = i == 0,
            .wrong_bssid = i == 1, .zero_ip = i == 2,
            .ready_after = i == 3 ? 100 : 0};
        target.bssid_set = 1;
        assert(h2_wifi_sta_connect_and_save(&deps, &target, 31) == H2_PAL_ERR_TIMEOUT);
        assert(f.now == 31 && !f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    }
    f = (fixture_t){.saved = old};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 0) == H2_PAL_ERR_INVALID_ARG);
    assert(!f.connects && !f.disconnects && !f.saves);
    assert(h2_pal_wifi_sta_connect_and_save(&sta, &target, 100) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_wifi_sta_connect_and_save(NULL, &target, 100) == H2_PAL_ERR_INVALID_ARG);
    target.ssid_len = H2_PAL_WIFI_SSID_MAX + 1;
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_INVALID_ARG);
    target = old;
    target.password[0] = 'x';
    f = (fixture_t){.saved = old, .status.state = H2_PAL_WIFI_STA_STATE_GOT_IP,
        .disconnect_ms = 30, .connect_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(f.disconnects == 1 && f.timeout == 70 && !memcmp(&f.saved, &old, sizeof(old)));
    f = (fixture_t){.saved = old, .status.state = H2_PAL_WIFI_STA_STATE_CONNECTED,
        .disconnect_ms = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_TIMEOUT);
    assert(!f.connects && !f.saves);
    f = (fixture_t){.saved = old, .connect_ms = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_TIMEOUT);
    assert(!f.saves);
    f = (fixture_t){.saved = old, .get_rc = H2_PAL_ERR_UNSUPPORTED};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_UNSUPPORTED);
    assert(!f.connects && !f.disconnects && !f.saves);
    f = (fixture_t){.saved = old, .status_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.connects && !f.saves);
    f = (fixture_t){.saved = old, .sleep_rc = H2_PAL_ERR_IO, .ready_after = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.saves);
}
int main(void) {
    run_cases();
    puts("Wi-Fi transaction tests passed");
    return 0;
}
