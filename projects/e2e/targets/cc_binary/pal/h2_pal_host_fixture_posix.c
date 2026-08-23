#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_pal_host_fixture.h"

#include "h2_corehttp.h"
#include "h2_coremqtt.h"
#include "h2_desktop_platform.h"
#include "h2_pal_host_local_peers.h"
#include "h2_wolfssl.h"

#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#else
#include "h2_linux_platform.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct h2_pal_host_fixture {
    h2_runtime_t runtime;
#if defined(__APPLE__)
    h2_darwin_host_fs_t *fs;
#else
    h2_linux_host_fs_t *fs;
#endif
    h2_pal_host_local_peers_t *peers;
    h2_corehttp_t *http;
    h2_coremqtt_t *mqtt;
    h2_pal_http_api_t http_api;
    h2_pal_mqtt_api_t mqtt_api;
    uint8_t mqtt_buffer[4096];
    char temp_path[256];
    int wolfssl_initialized;
    int system_event_initialized;
};

static h2_pal_result_t posix_create_providers(
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
    strcpy(fixture->temp_path, "/tmp/h2-pal-host-e2e-XXXXXX");
    if (mkdtemp(fixture->temp_path) == NULL) {
        free(fixture);
        return H2_PAL_ERR_IO;
    }
    const char *sources[] = {fixture->temp_path};
    const char *targets[] = {"/data"};
#if defined(__APPLE__)
    h2_pal_result_t result = (h2_pal_result_t)h2_darwin_host_fs_create(
        sources, targets, 1u, &fixture->fs);
    fixture->runtime.fs = h2_darwin_host_fs_api(fixture->fs);
    fixture->runtime.net = h2_darwin_net_api();
    fixture->runtime.netif = h2_darwin_netif_api();
    fixture->runtime.system_event = h2_darwin_system_event_api();
    h2_wolfssl_entropy_fn entropy = h2_darwin_entropy;
#else
    h2_pal_result_t result = (h2_pal_result_t)h2_linux_host_fs_create(
        sources, targets, 1u, &fixture->fs);
    fixture->runtime.fs = h2_linux_host_fs_api(fixture->fs);
    fixture->runtime.net = h2_linux_net_api();
    fixture->runtime.netif = h2_linux_netif_api();
    fixture->runtime.system_event = h2_linux_system_event_api();
    h2_wolfssl_entropy_fn entropy = h2_linux_entropy;
#endif
    fixture->runtime.mem = h2_desktop_platform_default_allocator();
    fixture->runtime.log = h2_desktop_platform_log_api();
    fixture->runtime.time = h2_desktop_platform_time_api();
    fixture->runtime.task = h2_desktop_platform_task_api();
    fixture->runtime.queue = h2_desktop_platform_queue_api();
    fixture->runtime.sync = h2_desktop_platform_sync_api();
    if (result == H2_PAL_OK) {
        h2_wolfssl_config_t config = {
            .mem = *fixture->runtime.mem,
            .entropy = entropy,
        };
        result = h2_wolfssl_init(&config);
        if (result == H2_PAL_OK) fixture->wolfssl_initialized = 1;
    }
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_pal_system_event_init(
            fixture->runtime.system_event);
        if (result == H2_PAL_OK) fixture->system_event_initialized = 1;
    }
    h2_pal_host_local_peer_endpoints_t endpoints;
    if (result == H2_PAL_OK) {
        result = h2_pal_host_local_peers_create(
            fixture_config, &fixture->peers, &endpoints);
    }
    if (result == H2_PAL_OK) {
        result = posix_create_providers(fixture, &endpoints);
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
    if (fixture->system_event_initialized) {
        h2_pal_system_event_deinit(fixture->runtime.system_event);
    }
    h2_pal_result_t cleanup = H2_PAL_OK;
    if (fixture->wolfssl_initialized) cleanup = h2_wolfssl_deinit();
    if (result == H2_PAL_OK) result = cleanup;
#if defined(__APPLE__)
    h2_darwin_host_fs_destroy(fixture->fs);
#else
    h2_linux_host_fs_destroy(fixture->fs);
#endif
    if (rmdir(fixture->temp_path) != 0 && result == H2_PAL_OK) {
        result = H2_PAL_ERR_IO;
    }
    free(fixture);
    return result;
}
