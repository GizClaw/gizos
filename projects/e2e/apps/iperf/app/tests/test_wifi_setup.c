#include "h2_iperf_e2e_wifi.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct fixture {
    h2_pal_wifi_sta_status_t status;
    h2_pal_wifi_sta_config_t saved;
    int status_rc, saved_rc;
    unsigned status_reads, saved_reads, mutations;
} fixture_t;

static int get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
    fixture_t *fixture = user;
    ++fixture->status_reads;
    *out_status = fixture->status;
    return fixture->status_rc;
}

static int get_saved(void *user, h2_pal_wifi_sta_config_t *out_config) {
    fixture_t *fixture = user;
    ++fixture->saved_reads;
    *out_config = fixture->saved;
    return fixture->saved_rc;
}

static int connect(void *user, const h2_pal_wifi_sta_config_t *config,
                   uint32_t timeout_ms) {
    (void)config;
    (void)timeout_ms;
    ++((fixture_t *)user)->mutations;
    return H2_PAL_ERR_INVALID_STATE;
}

static int save(void *user, const h2_pal_wifi_sta_config_t *config) {
    (void)config;
    ++((fixture_t *)user)->mutations;
    return H2_PAL_ERR_INVALID_STATE;
}

static int clear(void *user) {
    ++((fixture_t *)user)->mutations;
    return H2_PAL_ERR_INVALID_STATE;
}

static const h2_pal_wifi_sta_vtable_t sta_methods = {
    .get_status = get_status,
    .connect = connect,
    .connect_and_save = connect,
};
static const h2_pal_wifi_settings_vtable_t settings_methods = {
    .get_saved_sta_config = get_saved,
    .set_saved_sta_config = save,
    .clear_saved_sta_config = clear,
};

static int select_wifi(fixture_t *fixture,
                       h2_pal_wifi_sta_config_t *out_config,
                       int *out_connected) {
    const h2_pal_wifi_sta_api_t sta = {fixture, &sta_methods};
    const h2_pal_wifi_settings_api_t settings = {fixture, &settings_methods};
    return h2_iperf_e2e_select_wifi(&sta, &settings, out_config, out_connected);
}

int main(void) {
    const h2_pal_wifi_sta_status_t ipv4 = {
        .state = H2_PAL_WIFI_STA_STATE_GOT_IP,
        .ip_valid = 1,
        .ip = {.ip4 = 0xc0000201u},
    };
    const h2_pal_wifi_sta_config_t empty = {0};
    h2_pal_wifi_sta_config_t config;
    int connected = -1;
    fixture_t fixture = {.status = ipv4,
                         .saved_rc = H2_PAL_ERR_NOT_FOUND};
    memset(&config, 0xa5, sizeof(config));
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_OK);
    CHECK(connected == 1 && memcmp(&config, &empty, sizeof(config)) == 0);
    CHECK(fixture.status_reads == 1u && fixture.saved_reads == 0u);
    CHECK(fixture.mutations == 0u);

    fixture = (fixture_t){.saved_rc = H2_PAL_ERR_NOT_FOUND};
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_ERR_NOT_FOUND);
    CHECK(connected == 0 && fixture.saved_reads == 1u);
    /* A later temporary connection satisfies setup without saving a profile. */
    fixture.status = ipv4;
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_OK);
    CHECK(connected == 1 && fixture.saved_reads == 1u && !fixture.mutations);

    fixture = (fixture_t){0};
    memcpy(fixture.saved.ssid, "fixture", 7u);
    fixture.saved.ssid_len = 7u;
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_OK);
    CHECK(connected == 0 && fixture.saved_reads == 1u && !fixture.mutations);
    CHECK(memcmp(&config, &fixture.saved, sizeof(config)) == 0);

    /* An address alone, an invalid IPv4 flag, zero IPv4 and IPv6-only must
     * never allow this IPv4-only matrix to begin. */
    for (unsigned variant = 0; variant < 4u; ++variant) {
        fixture = (fixture_t){.status = ipv4,
                              .saved_rc = H2_PAL_ERR_NOT_FOUND};
        if (variant == 0u) fixture.status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
        if (variant == 1u) fixture.status.ip_valid = 0;
        if (variant >= 2u) fixture.status.ip.ip4 = 0u;
        if (variant == 3u) {
            fixture.status.ip.ip6_valid = 1;
            fixture.status.ip.ip6[0] = 0xfdu;
        }
        CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_ERR_NOT_FOUND);
        CHECK(connected == 0 && fixture.saved_reads == 1u && !fixture.mutations);
    }
    fixture = (fixture_t){.status_rc = H2_PAL_ERR_IO};
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_ERR_IO);
    CHECK(!fixture.saved_reads && !connected && !fixture.mutations);
    fixture = (fixture_t){.saved_rc = H2_PAL_ERR_UNSUPPORTED};
    fixture.saved.ssid_len = 7u;
    CHECK(select_wifi(&fixture, &config, &connected) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(!connected && memcmp(&config, &empty, sizeof(config)) == 0);

    memset(&config, 0xa5, sizeof(config));
    connected = 1;
    CHECK(h2_iperf_e2e_select_wifi(NULL, NULL, &config, &connected) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(!connected && memcmp(&config, &empty, sizeof(config)) == 0);
    CHECK(select_wifi(&fixture, NULL, &connected) == H2_PAL_ERR_INVALID_ARG);
    CHECK(select_wifi(&fixture, &config, NULL) == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
