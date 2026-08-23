#include "h2_desktop_app_support.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static h2_pal_mqtt_client_config_t client_config(uint8_t *buffer, size_t buffer_len) {
    h2_pal_mqtt_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.endpoint.host.data = "localhost";
    config.endpoint.host.len = strlen("localhost");
    config.endpoint.port = 1883u;
    config.client_id.data = "desktop-adapter-test";
    config.client_id.len = strlen("desktop-adapter-test");
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
    h2::desktop::OwnedNetworkServices services;
    assert(h2::desktop::open_network_services(true, false, &services) ==
           H2_PAL_OK);
    const h2_pal_mqtt_api_t *api = services.mqtt();
    assert(api != NULL);
    assert(api == services.mqtt());
    assert(api->user != NULL);
    assert(api->vtable != NULL);
    assert(api->vtable->open != NULL);
    assert(api->vtable->connect != NULL);
    assert(api->vtable->disconnect != NULL);
    assert(api->vtable->publish != NULL);
    assert(api->vtable->subscribe != NULL);
    assert(api->vtable->unsubscribe != NULL);
    assert(api->vtable->process != NULL);
    assert(api->vtable->close != NULL);

    h2_pal_mqtt_client_t *client = NULL;
    h2_pal_mqtt_client_config_t invalid_config;
    memset(&invalid_config, 0, sizeof(invalid_config));
    assert(h2_pal_mqtt_open(api, &invalid_config, &client) == H2_PAL_ERR_INVALID_ARG);
    assert(client == NULL);

    uint8_t buffer[512];
    h2_pal_mqtt_client_config_t config = client_config(buffer, sizeof(buffer));
    assert(h2_pal_mqtt_open(api, &config, &client) == H2_PAL_OK);
    assert(client != NULL);
    h2_pal_mqtt_close(api, client);
    return 0;
}
