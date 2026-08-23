#include "h2_smoke_gizclaw_ping_speed.h"

#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define H2_SMOKE_GIZCLAW_DEFAULT_WIFI_CONNECT_TIMEOUT_MS 30000u
#define H2_SMOKE_GIZCLAW_DEFAULT_SERVER_INFO_TIMEOUT_MS 10000u
#define H2_SMOKE_GIZCLAW_DEFAULT_POLL_WINDOW_MS 3000u
#define H2_SMOKE_GIZCLAW_POLL_SLICE_MS 100
#define H2_SMOKE_GIZCLAW_WIFI_CONNECT_ATTEMPTS 3u

typedef struct smoke_wifi_scan_state {
    int found;
} smoke_wifi_scan_state_t;

static int smoke_str_empty(h2_gizclaw_str_t value) {
    return value.data == NULL || value.len == 0u;
}

static void smoke_print_skip(const char *stage, int rc) {
    printf("SKIP gizclaw-ping-speed reason=server_info_unavailable stage=%s rc=%d\n", stage, rc);
}

static void smoke_print_fail(const char *stage, int rc) {
    printf("FAIL gizclaw-ping-speed stage=%s rc=%d\n", stage, rc);
}

static int smoke_get_u32_or_default(uint32_t value, uint32_t fallback) {
    return (int)(value == 0u ? fallback : value);
}

static int smoke_parse_server_time_ms(const uint8_t *body, size_t body_len, uint64_t *out_wall_ms) {
    static const char key[] = "\"server_time\":";
    const char *text = (const char *)body;
    size_t key_len = sizeof(key) - 1u;

    if (body == NULL || out_wall_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i + key_len < body_len; ++i) {
        if (memcmp(&text[i], key, key_len) != 0) {
            continue;
        }
        i += key_len;
        while (i < body_len &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
        uint64_t value = 0u;
        int seen_digit = 0;
        while (i < body_len && text[i] >= '0' && text[i] <= '9') {
            uint64_t digit = (uint64_t)(text[i] - '0');
            if (value > (UINT64_MAX - digit) / 10u) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            value = (value * 10u) + digit;
            seen_digit = 1;
            ++i;
        }
        if (!seen_digit) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        *out_wall_ms = value;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int smoke_wait_got_ip(
    const h2_pal_wifi_sta_api_t *wifi_sta,
    const h2_pal_time_api_t *time,
    uint32_t timeout_ms) {
    if (wifi_sta == NULL || time == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t start_ms = 0;
    int rc = h2_pal_time_get_monotonic_ms(time, &start_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (;;) {
        h2_pal_wifi_sta_status_t status;
        memset(&status, 0, sizeof(status));
        rc = h2_pal_wifi_sta_get_status(wifi_sta, &status);
        if (rc == H2_PAL_OK && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
            return H2_PAL_OK;
        }
        uint64_t now_ms = 0;
        rc = h2_pal_time_get_monotonic_ms(time, &now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (h2_pal_time_elapsed_ms(start_ms, now_ms) >= timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        (void)h2_pal_time_sleep_ms(time, H2_SMOKE_GIZCLAW_POLL_SLICE_MS);
    }
}

static bool smoke_on_wifi_scan_result(void *user, const h2_pal_wifi_scan_entry_t *entry) {
    smoke_wifi_scan_state_t *state = (smoke_wifi_scan_state_t *)user;
    if (state == NULL || entry == NULL) {
        return true;
    }
    state->found = 1;
    printf(
        "H2_SMOKE_GIZCLAW_WIFI_AP_FOUND ssid=%.*s channel=%u rssi=%d security=%d\n",
        (int)entry->ssid_len,
        entry->ssid,
        (unsigned)entry->channel,
        entry->rssi,
        (int)entry->security);
    return true;
}

static int smoke_scan_wifi_target(
    const h2_pal_wifi_sta_api_t *wifi_sta,
    const h2_pal_wifi_sta_config_t *saved,
    uint32_t timeout_ms) {
    h2_pal_wifi_scan_request_t request;
    smoke_wifi_scan_state_t state = {0};

    if (wifi_sta == NULL || saved == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(&request, 0, sizeof(request));
    request.ssid_len = saved->ssid_len;
    memcpy(request.ssid, saved->ssid, saved->ssid_len);
    request.ssid[request.ssid_len] = '\0';

    int rc = h2_pal_wifi_sta_scan(wifi_sta, &request, smoke_on_wifi_scan_result, &state, timeout_ms);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_GIZCLAW_WIFI_SCAN rc=%d\n", rc);
        return rc;
    }
    if (state.found == 0) {
        printf("H2_SMOKE_GIZCLAW_WIFI_AP_NOT_FOUND ssid=%.*s\n", (int)saved->ssid_len, saved->ssid);
    }
    return H2_PAL_OK;
}

static int smoke_ensure_wifi_connected(
    h2_runtime_t *runtime,
    const h2_smoke_gizclaw_ping_speed_config_t *config) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    int rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc == H2_PAL_OK && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
        return H2_PAL_OK;
    }

    h2_pal_wifi_sta_config_t saved;
    memset(&saved, 0, sizeof(saved));
    rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &saved);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)smoke_scan_wifi_target(
        runtime->wifi_sta,
        &saved,
        (uint32_t)smoke_get_u32_or_default(
            config->wifi_connect_timeout_ms,
            H2_SMOKE_GIZCLAW_DEFAULT_WIFI_CONNECT_TIMEOUT_MS));
    uint32_t connect_timeout_ms = (uint32_t)smoke_get_u32_or_default(
        config->wifi_connect_timeout_ms,
        H2_SMOKE_GIZCLAW_DEFAULT_WIFI_CONNECT_TIMEOUT_MS);
    for (uint32_t attempt = 0u; attempt < H2_SMOKE_GIZCLAW_WIFI_CONNECT_ATTEMPTS; ++attempt) {
        rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, connect_timeout_ms);
        if (rc == H2_PAL_OK) {
            return smoke_wait_got_ip(runtime->wifi_sta, runtime->time, connect_timeout_ms);
        }
        memset(&status, 0, sizeof(status));
        int status_rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
        if (status_rc == H2_PAL_OK) {
            printf(
                "H2_SMOKE_GIZCLAW_WIFI_STATUS attempt=%u state=%d disconnect_reason=%d channel=%u rssi=%d\n",
                (unsigned)(attempt + 1u),
                (int)status.state,
                status.disconnect_reason,
                (unsigned)status.channel,
                status.rssi);
        } else {
            printf(
                "H2_SMOKE_GIZCLAW_WIFI_STATUS attempt=%u rc=%d\n",
                (unsigned)(attempt + 1u),
                status_rc);
        }
        if (attempt + 1u < H2_SMOKE_GIZCLAW_WIFI_CONNECT_ATTEMPTS) {
            (void)h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
            (void)h2_pal_time_sleep_ms(runtime->time, 1000u);
        }
    }
    return rc;
}

static int smoke_server_info_preflight(
    h2_runtime_t *runtime,
    const h2_smoke_gizclaw_ping_speed_config_t *config) {
    char url[192];
    if (config->server_endpoint.len > 150u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int n = snprintf(
        url,
        sizeof(url),
        "http://%.*s/server-info",
        (int)config->server_endpoint.len,
        config->server_endpoint.data);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = H2_PAL_HTTP_GET;
    request.url.data = url;
    request.url.len = (size_t)n;
    request.timeout_ms = smoke_get_u32_or_default(
        config->server_info_timeout_ms,
        H2_SMOKE_GIZCLAW_DEFAULT_SERVER_INFO_TIMEOUT_MS);
    request.response_allocator = runtime->mem;
    request.allocator = runtime->mem;

    h2_pal_http_response_t response;
    h2_pal_http_response_reset(&response);
    int rc = h2_pal_http_request(runtime->http, &request, &response);
    if (rc == H2_PAL_OK && h2_pal_http_status_has_error(response.status_code)) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK) {
        uint64_t server_time_ms = 0u;
        rc = smoke_parse_server_time_ms(response.body, response.body_len, &server_time_ms);
        if (rc == H2_PAL_OK) {
            rc = h2_pal_time_set_wall_ms(runtime->time, server_time_ms);
        }
        if (rc == H2_PAL_OK) {
            printf("H2_SMOKE_GIZCLAW_TIME_SYNC_OK server_time_ms=%" PRIu64 "\n", server_time_ms);
        }
    }
    h2_pal_http_response_free(runtime->http, &response);
    return rc;
}

static int smoke_poll_window(
    h2_gizclaw_client_t *client,
    const h2_pal_time_api_t *time,
    uint32_t window_ms) {
    uint64_t start_ms = 0;
    int rc = h2_pal_time_get_monotonic_ms(time, &start_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint32_t effective_window = window_ms == 0u ? H2_SMOKE_GIZCLAW_DEFAULT_POLL_WINDOW_MS : window_ms;
    for (;;) {
        rc = h2_gizclaw_client_poll(client, H2_SMOKE_GIZCLAW_POLL_SLICE_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        uint64_t now_ms = 0;
        rc = h2_pal_time_get_monotonic_ms(time, &now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (h2_pal_time_elapsed_ms(start_ms, now_ms) >= effective_window) {
            return H2_PAL_OK;
        }
    }
}

h2_smoke_gizclaw_result_t h2_smoke_gizclaw_ping_speed_run(
    h2_runtime_t *runtime,
    const h2_smoke_gizclaw_ping_speed_config_t *config) {
    if (runtime == NULL || config == NULL ||
        runtime->mem == NULL ||
        runtime->http == NULL ||
        runtime->webrtc == NULL ||
        runtime->crypto == NULL ||
        runtime->time == NULL ||
        runtime->wifi_sta == NULL ||
        runtime->wifi_settings == NULL ||
        smoke_str_empty(config->server_endpoint) ||
        smoke_str_empty(config->private_key)) {
        smoke_print_skip("inputs", H2_PAL_ERR_INVALID_ARG);
        return H2_SMOKE_GIZCLAW_SKIP;
    }

    int rc = smoke_ensure_wifi_connected(runtime, config);
    if (rc != H2_PAL_OK) {
        smoke_print_skip("wifi", rc);
        return H2_SMOKE_GIZCLAW_SKIP;
    }
    rc = smoke_server_info_preflight(runtime, config);
    if (rc != H2_PAL_OK) {
        smoke_print_skip("server_info", rc);
        return H2_SMOKE_GIZCLAW_SKIP;
    }
    printf("H2_SMOKE_GIZCLAW_SERVER_INFO_OK endpoint=%.*s\n",
           (int)config->server_endpoint.len,
           config->server_endpoint.data);

    const h2_gizclaw_config_t gizclaw_config = {
        .server_endpoint = config->server_endpoint,
        .private_key = config->private_key,
        .cipher_mode = config->cipher_mode,
        .connect_timeout_ms = config->connect_timeout_ms,
        .allocator = runtime->mem,
        .http = runtime->http,
        .webrtc = runtime->webrtc,
        .crypto = runtime->crypto,
        .time = runtime->time,
        .log = runtime->log,
    };
    h2_gizclaw_client_t *client = NULL;
    rc = h2_gizclaw_client_init(&gizclaw_config, &client);
    if (rc == H2_PAL_OK) {
        rc = h2_gizclaw_client_connect(client);
    }
    if (rc == H2_PAL_OK) {
        rc = smoke_poll_window(client, runtime->time, config->poll_window_ms);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_gizclaw_client_ping(client);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_gizclaw_client_speedtest(client);
    }
    if (rc == H2_PAL_OK) {
        rc = smoke_poll_window(client, runtime->time, config->poll_window_ms);
    }
    int close_rc = client == NULL ? H2_PAL_OK : h2_gizclaw_client_close(client);
    h2_gizclaw_client_deinit(client);
    if (rc != H2_PAL_OK) {
        smoke_print_fail("run", rc);
        return H2_SMOKE_GIZCLAW_FAIL;
    }
    if (close_rc != H2_PAL_OK) {
        smoke_print_fail("close", close_rc);
        return H2_SMOKE_GIZCLAW_FAIL;
    }
    printf("PASS gizclaw-ping-speed endpoint=%.*s\n",
           (int)config->server_endpoint.len,
           config->server_endpoint.data);
    return H2_SMOKE_GIZCLAW_OK;
}
