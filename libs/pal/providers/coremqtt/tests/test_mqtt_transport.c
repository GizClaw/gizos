#include "h2_coremqtt.h"

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

    return 0;
}
