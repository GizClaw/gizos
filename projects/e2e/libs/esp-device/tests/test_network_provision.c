#ifdef NDEBUG
#undef NDEBUG
#endif
#include "network_provision.h"
#include <assert.h>

typedef struct observed {
    h2_pal_wifi_sta_state_t sta_state;
    h2_pal_wifi_ap_state_t ap_state;
    int saved, sta_error, ap_error, settings_error, connect_error;
    uint8_t ip_valid, ip6_valid;
    unsigned sta_reads, ap_reads, saved_reads, connects, saves, clears;
    unsigned writes, flushes, fail_write, short_write, fail_flush;
    size_t length;
    char output[2048];
    h2_pal_wifi_sta_config_t connected;
} observed_t;

static int sta_status(void *user, h2_pal_wifi_sta_status_t *out) {
    observed_t *s = user;
    ++s->sta_reads;
    memset(out, 0, sizeof(*out));
    out->state = s->sta_state;
    out->ip_valid = s->ip_valid;
    out->ip.ip6_valid = s->ip6_valid;
    return s->sta_error;
}
static int ap_status(void *user, h2_pal_wifi_ap_status_t *out) {
    observed_t *s = user;
    ++s->ap_reads;
    memset(out, 0, sizeof(*out));
    out->state = s->ap_state;
    return s->ap_error;
}
static int has_saved(void *user, int *out) {
    observed_t *s = user;
    ++s->saved_reads;
    *out = s->saved;
    return s->settings_error;
}
static int saved_config(void *user, h2_pal_wifi_sta_config_t *out) {
    observed_t *s = user;
    ++s->saved_reads;
    memset(out, 0, sizeof(*out));
    return H2_PAL_ERR_NOT_FOUND;
}
static int connect(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout_ms) {
    observed_t *s = user;
    ++s->connects;
    assert(timeout_ms == 20000u);
    /* Observation was fully written and drained before the radio mutation. */
    assert(s->writes == 1u && s->flushes == 1u);
    assert(strstr(s->output, "phase=eligible saved=0") != NULL);
    s->connected = *config;
    s->sta_state = H2_PAL_WIFI_STA_STATE_CONNECTED;
    /* Association intentionally supplies no IP; the suite owns that wait. */
    s->ip_valid = s->ip6_valid = 0u;
    return s->connect_error;
}
static int save(void *user, const h2_pal_wifi_sta_config_t *config) {
    observed_t *s = user;
    (void)config;
    ++s->saves;
    return H2_PAL_ERR_IO;
}
static int clear(void *user) {
    observed_t *s = user;
    ++s->clears;
    return H2_PAL_ERR_IO;
}
static int connect_and_save(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout_ms) {
    (void)timeout_ms;
    return save(user, config);
}
static int write_record(void *user, const void *data, size_t length, size_t *written, uint32_t timeout_ms) {
    observed_t *s = user;
    assert(timeout_ms == 5000u);
    ++s->writes;
    *written = 0u;
    if (s->writes == s->fail_write) return H2_PAL_ERR_TIMEOUT;
    if (s->writes == s->short_write) --length;
    assert(length + s->length < sizeof(s->output));
    memcpy(s->output + s->length, data, length);
    s->length += length;
    s->output[s->length] = '\0';
    *written = length;
    return H2_PAL_OK;
}
static int flush(void *user) {
    observed_t *s = user;
    ++s->flushes;
    return s->flushes == s->fail_flush ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static const h2_pal_wifi_sta_config_t credentials = {
    .ssid = "caller-network", .ssid_len = 14u,
    .password = "private-input", .password_len = 13u,
};
static const h2_pal_wifi_sta_vtable_t sta_methods = {
    .get_status = sta_status, .connect = connect, .connect_and_save = connect_and_save,
};
static const h2_pal_wifi_ap_vtable_t ap_methods = {.get_status = ap_status};
static const h2_pal_wifi_settings_vtable_t settings_methods = {
    .has_saved_sta_config = has_saved, .set_saved_sta_config = save, .clear_saved_sta_config = clear,
};
static const h2_pal_uart_io_stream_vtable_t uart_methods = {.write = write_record, .flush = flush};

static observed_t idle(void) {
    return (observed_t){.sta_state = H2_PAL_WIFI_STA_STATE_DISCONNECTED,
        .ap_state = H2_PAL_WIFI_AP_STATE_STOPPED};
}
static int provision(observed_t *s) {
    h2_pal_wifi_sta_api_t sta = {s, &sta_methods};
    h2_pal_wifi_ap_api_t ap = {s, &ap_methods};
    h2_pal_wifi_settings_api_t settings = {s, &settings_methods};
    h2_pal_uart_io_stream_api_t uart = {s, &uart_methods};
    h2_esp_e2e_console_t console = {.uart = &uart, .timeout_ms = 5000u};
    int rc = h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 20000u, true, &console);
    assert(s->saves == 0u && s->clears == 0u);
    assert(strstr(s->output, credentials.ssid) == NULL && strstr(s->output, credentials.password) == NULL);
    return rc;
}
static void test_disabled(void) {
    h2_pal_wifi_sta_config_t empty = {0};
    assert(h2_esp_e2e_provision_wifi(NULL, NULL, NULL, NULL, 0u, false, NULL) == H2_PAL_OK);
    assert(h2_esp_e2e_provision_wifi(NULL, NULL, NULL, &empty, 0u, false, NULL) == H2_PAL_OK);
}
static void test_borrow_existing(void) {
    const h2_pal_wifi_sta_state_t states[] = {H2_PAL_WIFI_STA_STATE_SCANNING,
        H2_PAL_WIFI_STA_STATE_CONNECTING, H2_PAL_WIFI_STA_STATE_CONNECTED, H2_PAL_WIFI_STA_STATE_GOT_IP};
    for (size_t i = 0u; i < sizeof(states) / sizeof(states[0]); ++i) {
        observed_t s = idle(); s.sta_state = states[i];
        assert(provision(&s) == H2_PAL_OK);
        assert(s.sta_reads == 1u && s.saved_reads == 0u && s.ap_reads == 0u && s.connects == 0u);
        assert(strstr(s.output, "phase=existing saved=-1") != NULL);
    }
    observed_t s = idle(); s.saved = 1;
    assert(provision(&s) == H2_PAL_OK);
    assert(s.saved_reads == 1u && s.ap_reads == 0u && s.connects == 0u);
    assert(strstr(s.output, "phase=saved saved=1") != NULL);
}
static void test_eligible(void) {
    const h2_pal_wifi_sta_state_t states[] = {H2_PAL_WIFI_STA_STATE_IDLE,
        H2_PAL_WIFI_STA_STATE_DISCONNECTED, H2_PAL_WIFI_STA_STATE_FAILED};
    for (size_t i = 0u; i < sizeof(states) / sizeof(states[0]); ++i) {
        observed_t s = idle(); s.sta_state = states[i];
        assert(provision(&s) == H2_PAL_OK);
        assert(s.sta_reads == 1u && s.saved_reads == 1u && s.ap_reads == 1u && s.connects == 1u);
        assert(s.writes == 2u && s.flushes == 2u && s.saved == 0);
        assert(s.connected.ssid_len == credentials.ssid_len && s.connected.password_len == credentials.password_len);
        assert(memcmp(s.connected.ssid, credentials.ssid, credentials.ssid_len) == 0);
        assert(memcmp(s.connected.password, credentials.password, credentials.password_len) == 0);
        assert(s.sta_state == H2_PAL_WIFI_STA_STATE_CONNECTED && !s.ip_valid && !s.ip6_valid);
        assert(strstr(s.output, "phase=connect saved=0") != NULL && strstr(s.output, "temporary=1 rc=0") != NULL);
    }
}
static void test_reject_unknown_or_failed_read(void) {
    observed_t s = idle(); s.sta_state = H2_PAL_WIFI_STA_STATE_UNKNOWN;
    assert(provision(&s) == H2_PAL_ERR_INVALID_STATE && s.connects == 0u);
    s = idle(); s.sta_state = (h2_pal_wifi_sta_state_t)99;
    assert(provision(&s) == H2_PAL_ERR_INVALID_STATE && s.connects == 0u);
    s = idle(); s.ip_valid = 1u;
    assert(provision(&s) == H2_PAL_ERR_INVALID_STATE && s.connects == 0u);
    s = idle(); s.ip6_valid = 1u;
    assert(provision(&s) == H2_PAL_ERR_INVALID_STATE && s.connects == 0u);
    const int errors[] = {H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_IO,
        H2_PAL_ERR_UNAVAILABLE, H2_PAL_ERR_NOT_FOUND};
    for (size_t i = 0u; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        s = idle(); s.sta_error = errors[i];
        assert(provision(&s) == errors[i] && s.saved_reads == 0u && s.connects == 0u);
        s = idle(); s.settings_error = errors[i];
        assert(provision(&s) == errors[i] && s.ap_reads == 0u && s.connects == 0u);
        assert(strstr(s.output, "phase=settings saved=-1") != NULL);
        if (errors[i] != H2_PAL_ERR_UNSUPPORTED) {
            s = idle(); s.ap_error = errors[i];
            assert(provision(&s) == errors[i] && s.connects == 0u);
        }
    }
    s = idle(); s.saved = -1;
    assert(provision(&s) == H2_PAL_ERR_FORMAT && s.connects == 0u);
    s = idle(); s.saved = 2;
    assert(provision(&s) == H2_PAL_ERR_FORMAT && s.connects == 0u);
    const h2_pal_wifi_ap_state_t active[] = {H2_PAL_WIFI_AP_STATE_STARTING,
        H2_PAL_WIFI_AP_STATE_STARTED, H2_PAL_WIFI_AP_STATE_STOPPING};
    for (size_t i = 0u; i < sizeof(active) / sizeof(active[0]); ++i) {
        s = idle(); s.ap_state = active[i];
        assert(provision(&s) == H2_PAL_ERR_BUSY && s.connects == 0u);
    }
    s = idle(); s.ap_state = H2_PAL_WIFI_AP_STATE_UNKNOWN;
    assert(provision(&s) == H2_PAL_ERR_INVALID_STATE && s.connects == 0u);
}
static void test_connect_and_output_failures(void) {
    observed_t s = idle(); s.connect_error = H2_PAL_ERR_TIMEOUT;
    assert(provision(&s) == H2_PAL_ERR_TIMEOUT && s.connects == 1u);
    assert(strstr(s.output, "temporary=1 rc=-") != NULL);
    for (unsigned call = 1u; call <= 2u; ++call) {
        s = idle(); s.fail_write = call;
        assert(provision(&s) == H2_PAL_ERR_TIMEOUT && s.connects == call - 1u);
        s = idle(); s.short_write = call;
        assert(provision(&s) == H2_PAL_ERR_IO && s.connects == call - 1u);
        s = idle(); s.fail_flush = call;
        assert(provision(&s) == H2_PAL_ERR_IO && s.connects == call - 1u);
    }
}
static void test_absent_capabilities_and_config(void) {
    observed_t s = idle();
    h2_pal_wifi_sta_api_t sta = {&s, &sta_methods};
    h2_pal_wifi_ap_api_t ap = {&s, &ap_methods};
    h2_pal_wifi_settings_api_t settings = {&s, &settings_methods};
    h2_pal_uart_io_stream_api_t uart = {&s, &uart_methods};
    h2_esp_e2e_console_t console = {.uart = &uart, .timeout_ms = 5000u};
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 0u, false, &console) == H2_PAL_ERR_INVALID_ARG);
    assert(s.sta_reads == 0u && s.connects == 0u);
    h2_pal_wifi_sta_config_t invalid = credentials; invalid.ssid_len = H2_PAL_WIFI_SSID_MAX + 1u;
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &invalid, 20000u, false, &console) == H2_PAL_ERR_INVALID_ARG);
    assert(s.sta_reads == 0u && s.connects == 0u);
    h2_pal_wifi_settings_vtable_t missing = {0}; settings.vtable = &missing;
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 20000u, false, &console) == H2_PAL_ERR_UNSUPPORTED);
    assert(s.connects == 0u);
    /* PAL's documented NOT_FOUND fallback is an actual empty observation. */
    h2_pal_wifi_settings_vtable_t fallback = {.get_saved_sta_config = saved_config};
    settings.vtable = &fallback; s = idle();
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 20000u, false, &console) == H2_PAL_OK);
    assert(s.connects == 1u && s.saves == 0u && s.clears == 0u);
    /* A capability disposition is explicit, never a fabricated stopped AP. */
    s = idle(); s.ap_error = H2_PAL_ERR_UNSUPPORTED;
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 20000u, false, &console) == H2_PAL_ERR_UNSUPPORTED);
    assert(s.connects == 0u);
    s = idle(); s.ap_error = H2_PAL_ERR_UNSUPPORTED;
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, &credentials, 20000u, true, &console) == H2_PAL_OK);
    assert(s.connects == 1u && s.saves == 0u && s.clears == 0u);
    assert(strstr(s.output, "pre_ap_state=-1 ap_capability=unsupported ap_rc=-3") != NULL);
}

static void test_public_fixture_mapping(void) {
    const h2_pal_wifi_sta_config_t *fixture = h2_esp_e2e_public_wifi_fixture();
#if defined(H2_E2E_WIFI_FIXTURE_HAIVIVI_MFG)
    assert(fixture != NULL && h2_pal_wifi_settings_validate_sta_config(fixture) == H2_PAL_OK);
    assert(fixture->ssid_len == strlen(fixture->ssid) && strcmp(fixture->ssid, "HAIVIVI-MFG") == 0);
    assert(fixture->password_len == strlen(fixture->password) && strcmp(fixture->password, "!haivivi") == 0);
    observed_t s = idle(); s.ap_error = H2_PAL_ERR_UNSUPPORTED;
    h2_pal_wifi_sta_api_t sta = {&s, &sta_methods};
    h2_pal_wifi_ap_api_t ap = {&s, &ap_methods};
    h2_pal_wifi_settings_api_t settings = {&s, &settings_methods};
    h2_pal_uart_io_stream_api_t uart = {&s, &uart_methods};
    h2_esp_e2e_console_t console = {.uart = &uart, .timeout_ms = 5000u};
    assert(h2_esp_e2e_provision_wifi(&sta, &ap, &settings, fixture, 20000u, true, &console) == H2_PAL_OK);
    assert(s.connects == 1u && s.saves == 0u && s.clears == 0u && !s.ip_valid);
    assert(s.connected.ssid_len == fixture->ssid_len && s.connected.password_len == fixture->password_len);
    assert(memcmp(s.connected.ssid, fixture->ssid, fixture->ssid_len) == 0);
    assert(memcmp(s.connected.password, fixture->password, fixture->password_len) == 0);
    assert(strstr(s.output, fixture->ssid) == NULL && strstr(s.output, fixture->password) == NULL);
#else
    assert(fixture == NULL);
    assert(h2_esp_e2e_provision_wifi(NULL, NULL, NULL, fixture, 0u, false, NULL) == H2_PAL_OK);
#endif
}

int main(void) {
    test_disabled();
    test_borrow_existing();
    test_eligible();
    test_reject_unknown_or_failed_read();
    test_connect_and_output_failures();
    test_absent_capabilities_and_config();
    test_public_fixture_mapping();
    return 0;
}
