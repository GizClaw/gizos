#include "h2_pal_http_network.h"
#include <string.h>

typedef struct fixture {
    int connected, saved_result;
    unsigned statuses, saved_reads, connects;
} fixture_t;
static int status(void *user, h2_pal_wifi_sta_status_t *out) {
    fixture_t *fixture = user;
    ++fixture->statuses;
    memset(out, 0, sizeof(*out));
    if (fixture->connected) {
        out->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
        out->ip_valid = 1;
        out->ip.ip4 = 1u;
    }
    return H2_PAL_OK;
}
static int saved(void *user, h2_pal_wifi_sta_config_t *out) {
    fixture_t *fixture = user;
    ++fixture->saved_reads;
    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, "fixture", 7u);
    out->ssid_len = 7u;
    return fixture->saved_result;
}
static int connect(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout) {
    fixture_t *fixture = user;
    ++fixture->connects;
    if (config->ssid_len != 7u || timeout == 0u) return H2_PAL_ERR_INVALID_ARG;
    fixture->connected = 1;
    return H2_PAL_OK;
}
static h2_pal_result_t clock_ms(void *user, uint64_t *out) {
    (void)user; *out = 1u; return H2_PAL_OK;
}
int main(void) {
    fixture_t fixture = {.connected = 1, .saved_result = H2_PAL_ERR_NOT_FOUND};
    h2_pal_wifi_sta_vtable_t sta_methods = {.get_status = status, .connect = connect};
    h2_pal_wifi_settings_vtable_t settings_methods = {.get_saved_sta_config = saved};
    h2_pal_time_vtable_t time_methods = {.get_monotonic_ms = clock_ms};
    h2_pal_wifi_sta_api_t sta = {&fixture, &sta_methods};
    h2_pal_wifi_settings_api_t settings = {&fixture, &settings_methods};
    h2_pal_time_api_t time = {NULL, &time_methods};
    h2_runtime_t runtime = {.wifi_sta = &sta, .wifi_settings = &settings, .time = &time};
    if (h2_pal_http_device_prepare_network(&runtime) != H2_PAL_OK ||
        fixture.statuses != 1u || fixture.saved_reads != 0u || fixture.connects != 0u) return 1;
    fixture = (fixture_t){.saved_result = H2_PAL_ERR_NOT_FOUND};
    if (h2_pal_http_device_prepare_network(&runtime) != H2_PAL_ERR_NOT_FOUND ||
        fixture.saved_reads != 1u || fixture.connects != 0u) return 2;
    fixture = (fixture_t){.saved_result = H2_PAL_OK};
    if (h2_pal_http_device_prepare_network(&runtime) != H2_PAL_OK ||
        fixture.saved_reads != 1u || fixture.connects != 1u || fixture.statuses != 2u) return 3;
    return 0;
}
