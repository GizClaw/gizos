#include "h2_coremqtt.h"

#include "fake_mqtt_platform.h"

#include <assert.h>
#include <string.h>

static h2_pal_mqtt_client_config_t client_config(uint8_t *buffer, size_t buffer_len) {
    h2_pal_mqtt_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.endpoint.host.data = "localhost";
    config.endpoint.host.len = strlen("localhost");
    config.endpoint.port = 1883u;
    config.client_id.data = "api-test";
    config.client_id.len = strlen("api-test");
    config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    config.keepalive_sec = 30u;
    config.connect_timeout_ms = 100u;
    config.operation_timeout_ms = 50u;
    config.clean_session = 1;
    config.network_buffer = buffer;
    config.network_buffer_len = buffer_len;
    return config;
}

int main(void) {
    fake_mqtt_platform_t fake;
    fake_mqtt_platform_init(&fake);

    h2_coremqtt_config_t provider_config;
    memset(&provider_config, 0, sizeof(provider_config));
    provider_config.allocator = &fake.allocator;
    provider_config.net = &fake.net;
    provider_config.time = &fake.time;

    h2_coremqtt_t *provider = NULL;
    h2_pal_mqtt_api_t api;
    memset(&api, 0, sizeof(api));
    assert(h2_coremqtt_create(&provider_config, &provider, &api) == H2_PAL_OK);
    assert(api.user == provider);
    assert(api.vtable != NULL);

    uint8_t buffer[512];
    h2_pal_mqtt_client_config_t config = client_config(buffer, sizeof(buffer));
    h2_pal_mqtt_client_t *client = NULL;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(client != NULL);

    h2_pal_mqtt_publish_t publish;
    memset(&publish, 0, sizeof(publish));
    publish.topic.data = "topic";
    publish.topic.len = strlen("topic");
    publish.qos = (h2_pal_mqtt_qos_t)2;
    assert(api.vtable->publish(api.user, client, &publish, NULL) == H2_PAL_ERR_UNSUPPORTED);

    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);
    return 0;
}
