#include "h2_pal_host_fixture.h"

#include "h2_corehttp.h"
#include "h2_coremqtt.h"
#include "h2_pal_host_local_peers.h"
#include "h2_windows_platform.h"
#include "h2_wolfssl.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct h2_pal_host_fixture {
    h2_runtime_t runtime;
    h2_windows_platform_t *platform;
    h2_pal_host_local_peers_t *peers;
    h2_corehttp_t *http;
    h2_coremqtt_t *mqtt;
    h2_pal_http_api_t http_api;
    h2_pal_mqtt_api_t mqtt_api;
    uint8_t mqtt_buffer[4096];
    wchar_t temp_path[MAX_PATH];
    int wolfssl_initialized;
    int system_event_initialized;
};

static h2_pal_result_t windows_make_temp(h2_pal_host_fixture_t *fixture,
                                         char *out_utf8,
                                         size_t utf8_capacity) {
    wchar_t root[MAX_PATH];
    wchar_t candidate[MAX_PATH];
    if (GetTempPathW((DWORD)(sizeof(root) / sizeof(root[0])), root) == 0u ||
        GetTempFileNameW(root, L"h2p", 0u, candidate) == 0u ||
        !DeleteFileW(candidate) || !CreateDirectoryW(candidate, NULL) ||
        wcsncpy_s(fixture->temp_path,
                  sizeof(fixture->temp_path) / sizeof(fixture->temp_path[0]),
                  candidate, _TRUNCATE) != 0 ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, candidate, -1,
                            out_utf8, (int)utf8_capacity, NULL, NULL) <= 0) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t windows_create_providers(
    h2_pal_host_fixture_t *fixture,
    const h2_pal_host_local_peer_endpoints_t *endpoints) {
    h2_corehttp_config_t http_config = {
        .allocator = fixture->runtime.mem,
        .net = fixture->runtime.net,
        .time = fixture->runtime.time,
        .log = fixture->runtime.log,
        .root_ca_pem = endpoints->root_ca_pem,
        .root_ca_pem_len = endpoints->root_ca_pem_len,
        .default_timeout_ms = 3000u,
        .io_slice_ms = 20u,
    };
    h2_pal_result_t result = h2_corehttp_create(
        &http_config, &fixture->http, &fixture->http_api);
    h2_coremqtt_config_t mqtt_config = {
        .allocator = fixture->runtime.mem,
        .net = fixture->runtime.net,
        .time = fixture->runtime.time,
        .log = fixture->runtime.log,
        .outgoing_publish_records = 4u,
        .incoming_publish_records = 4u,
    };
    if (result == H2_PAL_OK) {
        result = h2_coremqtt_create(&mqtt_config, &fixture->mqtt,
                                    &fixture->mqtt_api);
    }
    if (result == H2_PAL_OK) {
        fixture->runtime.http = &fixture->http_api;
        fixture->runtime.mqtt = &fixture->mqtt_api;
    }
    return result;
}

h2_pal_result_t h2_pal_host_fixture_create(
    const h2_pal_host_fixture_config_t *fixture_config,
    h2_pal_host_fixture_t **out_fixture, h2_runtime_t **out_runtime,
    h2_pal_e2e_config_t *out_config) {
    if (fixture_config == NULL || out_fixture == NULL || out_runtime == NULL ||
        out_config == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_fixture = NULL;
    *out_runtime = NULL;
    memset(out_config, 0, sizeof(*out_config));
    h2_pal_host_fixture_t *fixture = calloc(1u, sizeof(*fixture));
    if (fixture == NULL) return H2_PAL_ERR_NO_MEMORY;
    char temp_utf8[MAX_PATH * 4u];
    h2_pal_result_t result = windows_make_temp(
        fixture, temp_utf8, sizeof(temp_utf8));
    const char *sources[] = {temp_utf8};
    const char *targets[] = {"/data"};
    h2_windows_platform_config_t platform_config = {
        .fs_sources = sources,
        .fs_targets = targets,
        .fs_mount_count = 1u,
    };
    if (result == H2_PAL_OK) {
        result = h2_windows_platform_create(&platform_config,
                                             &fixture->platform);
    }
    if (result == H2_PAL_OK) {
        fixture->runtime.mem = h2_windows_mem_api(fixture->platform);
        fixture->runtime.log = h2_windows_log_api(fixture->platform);
        fixture->runtime.time = h2_windows_time_api(fixture->platform);
        fixture->runtime.timer = h2_windows_timer_api(fixture->platform);
        fixture->runtime.task = h2_windows_task_api(fixture->platform);
        fixture->runtime.queue = h2_windows_queue_api(fixture->platform);
        fixture->runtime.sync = h2_windows_sync_api(fixture->platform);
        fixture->runtime.fs = h2_windows_fs_api(fixture->platform);
        fixture->runtime.net = h2_windows_net_api(fixture->platform);
        fixture->runtime.netif = h2_windows_netif_api(fixture->platform);
        fixture->runtime.system_event =
            h2_windows_system_event_api(fixture->platform);
        h2_wolfssl_config_t wolfssl_config = {
            .mem = *fixture->runtime.mem,
            .entropy_user = fixture->platform,
            .entropy = h2_windows_entropy,
        };
        result = h2_wolfssl_init(&wolfssl_config);
        if (result == H2_PAL_OK) fixture->wolfssl_initialized = 1;
    }
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_pal_system_event_init(
            fixture->runtime.system_event);
        if (result == H2_PAL_OK) fixture->system_event_initialized = 1;
    }
    h2_pal_host_local_peer_endpoints_t endpoints = {0};
    if (result == H2_PAL_OK) {
        result = h2_pal_host_local_peers_create(
            fixture_config, &fixture->peers, &endpoints);
    }
    if (result == H2_PAL_OK) {
        result = windows_create_providers(fixture, &endpoints);
    }
    if (result != H2_PAL_OK) {
        (void)h2_pal_host_fixture_destroy(fixture);
        return result;
    }
    out_config->suite_mask = H2_PAL_E2E_SUITE_HOST;
    out_config->host.tcp_echo_port = endpoints.tcp_echo_port;
    out_config->host.tls_echo_port = endpoints.tls_echo_port;
    out_config->host.tls_wrong_ca_port = endpoints.tls_wrong_ca_port;
    out_config->host.https_port = endpoints.https_port;
    out_config->host.timeout_ms = 3000u;
    out_config->host.root_ca_pem = endpoints.root_ca_pem;
    out_config->host.root_ca_pem_len = endpoints.root_ca_pem_len;
    out_config->host.wrong_ca_pem = endpoints.wrong_ca_pem;
    out_config->host.wrong_ca_pem_len = endpoints.wrong_ca_pem_len;
    out_config->mqtt.host = "127.0.0.1";
    out_config->mqtt.port = endpoints.mqtt_port;
    out_config->mqtt.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    out_config->mqtt.client_id = (h2_pal_mqtt_str_t){"h2-host-e2e", 11u};
    out_config->mqtt.topic = (h2_pal_mqtt_str_t){"h2/host/e2e", 11u};
    out_config->mqtt.payload =
        (h2_pal_mqtt_bytes_t){(const uint8_t *)"payload", 7u};
    out_config->mqtt.timeout_ms = 3000u;
    out_config->mqtt.network_buffer = fixture->mqtt_buffer;
    out_config->mqtt.network_buffer_len = sizeof(fixture->mqtt_buffer);
    *out_fixture = fixture;
    *out_runtime = &fixture->runtime;
    return H2_PAL_OK;
}

h2_pal_result_t h2_pal_host_fixture_destroy(
    h2_pal_host_fixture_t *fixture) {
    if (fixture == NULL) return H2_PAL_OK;
    h2_coremqtt_destroy(fixture->mqtt);
    h2_corehttp_destroy(fixture->http);
    h2_pal_result_t result = h2_pal_host_local_peers_destroy(fixture->peers);
    if (result == H2_PAL_ERR_BUSY) return result;
    if (fixture->system_event_initialized) {
        h2_pal_system_event_deinit(fixture->runtime.system_event);
    }
    h2_pal_result_t cleanup = H2_PAL_OK;
    if (fixture->wolfssl_initialized) cleanup = h2_wolfssl_deinit();
    if (result == H2_PAL_OK) result = cleanup;
    cleanup = h2_windows_platform_destroy(&fixture->platform);
    if (result == H2_PAL_OK) result = cleanup;
    if (fixture->temp_path[0] != L'\0' &&
        !RemoveDirectoryW(fixture->temp_path) && result == H2_PAL_OK) {
        result = H2_PAL_ERR_IO;
    }
    free(fixture);
    return result;
}
