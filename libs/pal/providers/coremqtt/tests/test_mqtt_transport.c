#include "h2_coremqtt.h"
#include "h2_coremqtt_internal.h"

#include "fake_mqtt_platform.h"

#include <assert.h>
#include <string.h>

static h2_pal_mqtt_client_config_t base_config(uint8_t *buffer, size_t buffer_len) {
    h2_pal_mqtt_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.endpoint.host.data = "localhost";
    config.endpoint.host.len = strlen("localhost");
    config.endpoint.port = 8883u;
    config.client_id.data = "transport-test";
    config.client_id.len = strlen("transport-test");
    config.keepalive_sec = 30u;
    config.connect_timeout_ms = 100u;
    config.operation_timeout_ms = 50u;
    config.clean_session = 1;
    config.network_buffer = buffer;
    config.network_buffer_len = buffer_len;
    return config;
}

static h2_pal_mqtt_api_t make_api(fake_mqtt_platform_t *fake, h2_coremqtt_t **out_provider) {
    h2_coremqtt_config_t provider_config;
    memset(&provider_config, 0, sizeof(provider_config));
    provider_config.allocator = &fake->allocator;
    provider_config.net = &fake->net;
    provider_config.time = &fake->time;
    h2_pal_mqtt_api_t api;
    assert(h2_coremqtt_create(&provider_config, out_provider, &api) == H2_PAL_OK);
    return api;
}

int main(void) {
    uint8_t buffer[1024];
    h2_pal_net_tls_config_t tls;
    memset(&tls, 0, sizeof(tls));
    tls.server_name = "localhost";
    tls.verify = H2_PAL_NET_TLS_VERIFY_REQUIRED;

    fake_mqtt_platform_t fake;
    fake_mqtt_platform_init(&fake);
    fake.tls_supported = 0;
    h2_coremqtt_t *provider = NULL;
    h2_pal_mqtt_api_t api = make_api(&fake, &provider);
    h2_pal_mqtt_client_config_t config = base_config(buffer, sizeof(buffer));
    config.transport = H2_PAL_MQTT_TRANSPORT_TLS;
    config.tls = &tls;
    h2_pal_mqtt_client_t *client = NULL;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_ERR_UNSUPPORTED);
    assert(fake.tls_called == 1);
    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);

    fake_mqtt_platform_init(&fake);
    fake.tls_supported = 1;
    fake.tls_verify_fail = 1;
    provider = NULL;
    api = make_api(&fake, &provider);
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_ERR_TLS_VERIFY);
    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);

    fake_mqtt_platform_init(&fake);
    fake.timeout_connect = 1;
    provider = NULL;
    api = make_api(&fake, &provider);
    config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    config.tls = NULL;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_ERR_TIMEOUT);
    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);

    /* A successful blocking PAL write can exceed coreMQTT's 10 ms retry
     * window. The complete CONNECT vector must still reach the wire; sending
     * only its header cannot elicit the parser's real CONNACK response. */
    fake_mqtt_platform_init(&fake);
    fake.send_delay_ms = 25u;
    provider = NULL;
    api = make_api(&fake, &provider);
    config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    config.tls = NULL;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_OK);
    assert(fake.tx_len > 15u && fake.tx_scan_pos == fake.tx_len);
    assert(fake.now_ms >= 75u);
    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);

    /* Both slow short prefixes and a refused write after a prefix must
     * complete the real coreMQTT CONNECT before the configured deadline. */
    for (unsigned blocked = 0u; blocked < 2u; ++blocked) {
        fake_mqtt_platform_init(&fake);
        fake.send_delay_ms = 25u;
        fake.send_limit = 3u;
        if (blocked) {
            fake.send_error = H2_PAL_ERR_WOULD_BLOCK;
            fake.send_error_after = 1u;
            fake.send_error_once = 1;
        }
        provider = NULL;
        api = make_api(&fake, &provider);
        config.connect_timeout_ms = 1000u;
        assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
        assert(api.vtable->connect(api.user, client) == H2_PAL_OK);
        assert(fake.tx_scan_pos == fake.tx_len && fake.tx_len > 15u);
        assert(fake.now_ms >= 250u && fake.now_ms < 1000u);
        assert(fake.send_timeouts[0] >= 999u && fake.send_timeouts[0] <= 1000u);
        assert(fake.send_timeouts[1] < fake.send_timeouts[0]);
        api.vtable->close(api.user, client);
        h2_coremqtt_destroy(provider);
    }
    fake_mqtt_platform_init(&fake);
    fake.send_delay_ms = 25u;
    fake.send_limit = 3u;
    provider = NULL;
    api = make_api(&fake, &provider);
    config.connect_timeout_ms = 60u;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_ERR_TIMEOUT);
    assert(fake.now_ms >= 60u && fake.now_ms <= 70u);
    assert(fake.tx_scan_pos == 0u && fake.tx_len > 0u && fake.close_count == 1);
    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);

    fake_mqtt_platform_init(&fake);
    fake.auto_respond = 0;
    provider = NULL;
    api = make_api(&fake, &provider);
    h2_pal_mqtt_client_t transport_client = {.provider = provider, .socket = 1, .tls_socket = -1,
        .time_api = &fake.time, .send_timeout_ms = 100u};
    NetworkContext_t network = {.client = &transport_client};
    const uint8_t first[] = {1u, 2u, 3u}, second[] = {4u, 5u, 6u, 7u};
    TransportOutVector_t vectors[] = {{first, sizeof(first)}, {NULL, 0u}, {second, sizeof(second)}};
    fake.send_limit = 2u;
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == 7);
    assert(fake.tx_len == 7u && fake.send_calls == 4u);
    fake.tx_len = fake.send_calls = 0u;
    h2_coremqtt_begin_send(&transport_client, 100u);
    fake.send_limit = 0u;
    fake.send_error = H2_PAL_ERR_IO;
    fake.send_error_after = 1u;
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == 3);
    assert(fake.tx_len == 3u && fake.send_calls == 2u);
    assert(memcmp(fake.tx, first, sizeof(first)) == 0);
    fake.send_calls = 0u;
    h2_coremqtt_begin_send(&transport_client, 100u);
    fake.send_error_after = 0u;
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == -1);
    fake.send_error = H2_PAL_ERR_WOULD_BLOCK;
    uint64_t blocked_started = fake.now_ms;
    h2_coremqtt_begin_send(&transport_client, 100u);
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == -1);
    assert(transport_client.send_result == H2_PAL_ERR_TIMEOUT && fake.now_ms - blocked_started >= 100u && fake.now_ms - blocked_started <= 103u);
    fake.send_calls = 0u;
    vectors[2].iov_base = NULL;
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == -1);
    assert(fake.send_calls == 0u);
    vectors[2].iov_base = second;
    vectors[2].iov_len = (size_t)INT32_MAX;
    assert(h2_coremqtt_transport_writev(&network, vectors, 3u) == -1);
    assert(fake.send_calls == 0u);
    /* One operation spans multiple transport callbacks; neither callback
     * nor vector may renew its original deadline. */
    fake.tx_len = fake.send_calls = fake.send_timeout_calls = 0u;
    fake.send_error = 0;fake.send_delay_ms = 25u;fake.send_limit = 0u;
    h2_coremqtt_begin_send(&transport_client, 60u);
    assert(h2_coremqtt_transport_send(&network, first, sizeof(first)) == 3);
    assert(h2_coremqtt_transport_send(&network, second, sizeof(second)) == 4);
    assert(h2_coremqtt_transport_send(&network, first, sizeof(first)) == -1);
    assert(transport_client.send_result == H2_PAL_ERR_TIMEOUT && fake.tx_len == 7u);
    assert(fake.send_timeouts[1] < fake.send_timeouts[0] && fake.send_timeouts[2] < fake.send_timeouts[1]);
    unsigned calls = fake.send_calls;
    assert(h2_coremqtt_transport_send(&network, first, sizeof(first)) == -1 && fake.send_calls == calls);
    h2_coremqtt_begin_send(&transport_client, 0u);
    assert(transport_client.send_timeout_ms == 1000u);
    assert(h2_coremqtt_transport_send(&network, NULL, 0u) == 0 && !transport_client.send_deadline_active);
    transport_client.config.operation_timeout_ms = 75u;
    h2_coremqtt_begin_send(&transport_client, 0u);
    assert(transport_client.send_timeout_ms == 75u);
    transport_client.send_deadline_ms = fake.now_ms;
    transport_client.send_deadline_active = 1;
    assert(h2_coremqtt_transport_send(&network, first, sizeof(first)) == -1 && fake.send_calls == calls);
    h2_coremqtt_destroy(provider);
    assert(fake.live_allocations == 0u);

    /* The real vendor ProcessLoop must generate PINGREQ while a short poll
     * does not shorten the send budget. PINGRESP comes from the wire parser. */
    fake_mqtt_platform_init(&fake);provider = NULL;api = make_api(&fake, &provider);
    config = base_config(buffer, sizeof(buffer));config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    config.keepalive_sec = 1u;config.operation_timeout_ms = 100u;
    assert(api.vtable->open(api.user, &config, &client) == 0 && api.vtable->connect(api.user, client) == 0);
    fake.now_ms += 1200u;fake.send_delay_ms = 25u;fake.send_limit = 1u;
    size_t ping_start = fake.tx_len;fake.send_timeout_calls = 0u;
    assert(api.vtable->process(api.user, client, 1u) == H2_PAL_OK);
    assert(fake.tx_len == ping_start + 2u && fake.tx[ping_start] == 0xc0u && fake.tx[ping_start+1u] == 0u);
    assert(client->connected && fake.close_count == 0 && fake.recv_timeout_ms == 1u);
    assert(fake.send_timeouts[0] >= 99u && fake.send_timeouts[1] < fake.send_timeouts[0]);
    assert(api.vtable->process(api.user, client, 1u) == 0 && client->connected);
    api.vtable->close(api.user, client);h2_coremqtt_destroy(provider);
    assert(fake.live_allocations == 0u && !fake.socket_open && fake.legacy_send_calls == 0u);

    /* Optional in generic Net PAL, timed send is required by this provider.
     * A missing capability or a real UNSUPPORTED result must not call the
     * unbounded legacy callback, send a CONNECT prefix, or leak its socket. */
    for (unsigned missing = 0u; missing < 2u; ++missing) {
        fake_mqtt_platform_init(&fake);provider = NULL;
        h2_pal_net_vtable_t selected = *fake.net.vtable;
        if (missing) selected.tcp_send_timeout = NULL;
        else fake.send_error = H2_PAL_ERR_UNSUPPORTED;
        fake.net.vtable = &selected;api = make_api(&fake, &provider);
        config = base_config(buffer, sizeof(buffer));config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
        assert(api.vtable->open(api.user, &config, &client) == 0);
        assert(api.vtable->connect(api.user, client) == H2_PAL_ERR_UNSUPPORTED);
        assert(fake.tx_len == 0u && fake.legacy_send_calls == 0u && !fake.socket_open && fake.close_count == 1);
        api.vtable->close(api.user, client);h2_coremqtt_destroy(provider);
        assert(fake.live_allocations == 0u);
    }
    return 0;

}
