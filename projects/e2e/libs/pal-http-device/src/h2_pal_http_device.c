#include "h2_pal_http_device.h"
#include "h2_corehttp.h"
#include "h2_pal_http_fixture_config.h"
#include <stdio.h>
#include <string.h>

const char h2_pal_http_device_runner_task_name[] = "pal-http/e2e/runner";

static void report(void *user, const h2_pal_http_e2e_case_result_t *result) {
    const h2_runtime_t *runtime = user;
    char line[256];
    (void)snprintf(line, sizeof(line), "H2_PAL_HTTP_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line);
    (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-http", line);
    if (runtime != NULL) (void)h2_pal_time_sleep_ms(runtime->time, 40u);
}

void h2_pal_http_device_report(const h2_runtime_t *runtime, const h2_pal_http_e2e_result_t *result) {
    for (unsigned index = 0u; index < H2_PAL_HTTP_E2E_CASE_COUNT; ++index)
        if (result->cases[index].id != NULL) report((void *)runtime, &result->cases[index]);
    char line[256];
    (void)snprintf(line, sizeof(line), "H2_PAL_HTTP_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"cleanup\":0}\n",
        result->passed, result->failed, result->blocked, result->retained_allocations);
    (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-http", line);
}

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int connect_saved_wifi(h2_runtime_t *runtime) {
    h2_pal_wifi_sta_config_t wifi = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &wifi);
    if (rc == H2_PAL_OK) rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &wifi, 20000u);
    memset(&wifi, 0, sizeof(wifi));
    if (rc != H2_PAL_OK) return rc;
    uint64_t start = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
    while (rc == H2_PAL_OK) {
        h2_pal_wifi_sta_status_t status = {0};
        rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
        if (rc != H2_PAL_OK) return rc;
        if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid)
            return H2_PAL_OK;
        uint64_t now = 0u;
        rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
        if (now - start >= 20000u) return H2_PAL_ERR_TIMEOUT;
        if (rc == H2_PAL_OK) rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    }
    return rc;
}

int h2_pal_http_device_run(h2_runtime_t *runtime, h2_pal_http_e2e_result_t *result) {
    if (runtime == NULL || result == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    const char *hex = H2_PAL_HTTP_CA_PEM_HEX;
    size_t hex_len = strlen(hex);
    if (hex_len == 0u || hex_len > 16384u || hex_len % 2u != 0u ||
        H2_PAL_HTTP_HTTP_BASE[0] == '\0' || H2_PAL_HTTP_HTTPS_BASE[0] == '\0' ||
        H2_PAL_HTTP_UNTRUSTED_HTTPS_BASE[0] == '\0' || H2_PAL_HTTP_FIXTURE_EPOCH_MS == 0u)
        return H2_PAL_ERR_INVALID_ARG;
    uint8_t *ca = h2_pal_mem_alloc(runtime->mem, hex_len / 2u);
    if (ca == NULL) return H2_PAL_ERR_NO_MEMORY;
    int rc = H2_PAL_OK;
    for (size_t index = 0u; index < hex_len; index += 2u) {
        int high = hex_digit(hex[index]), low = hex_digit(hex[index + 1u]);
        if (high < 0 || low < 0) { rc = H2_PAL_ERR_INVALID_ARG; break; }
        ca[index / 2u] = (uint8_t)((high << 4) | low);
    }
    if (rc == H2_PAL_OK) rc = connect_saved_wifi(runtime);
    if (rc == H2_PAL_OK)
        rc = h2_pal_time_set_wall_ms(runtime->time, H2_PAL_HTTP_FIXTURE_EPOCH_MS);
    h2_corehttp_t *provider = NULL;
    h2_pal_http_api_t api = {0};
    h2_corehttp_config_t provider_config = {
        .allocator = runtime->mem, .net = runtime->net, .time = runtime->time,
        .root_ca_pem = ca, .root_ca_pem_len = hex_len / 2u,
        .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED, .io_slice_ms = 20u,
    };
    if (rc == H2_PAL_OK) rc = h2_corehttp_create(&provider_config, &provider, &api);
    h2_pal_mem_free(runtime->mem, ca);
    if (rc != H2_PAL_OK) return rc;
    h2_runtime_t test_runtime = *runtime;
    test_runtime.http = &api;
    h2_pal_http_e2e_config_t config = {
        .runtime = &test_runtime, .http_base = H2_PAL_HTTP_HTTP_BASE,
        .https_base = H2_PAL_HTTP_HTTPS_BASE,
        .untrusted_https_base = H2_PAL_HTTP_UNTRUSTED_HTTPS_BASE,
        .report = report, .report_user = runtime,
    };
    rc = h2_pal_http_e2e_run(&config, result);
    h2_corehttp_destroy(provider);
    return rc;
}
