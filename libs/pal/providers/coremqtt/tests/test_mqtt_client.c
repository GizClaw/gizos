#include "h2_coremqtt.h"

#include "fake_mqtt_platform.h"

#include <assert.h>
#include <string.h>

typedef struct events {
    int connected;
    int publish_ack;
    int subscribe_ack;
    int unsubscribe_ack;
    int publish_received;
    int disconnected;
} events_t;

static void on_event(void *user, h2_pal_mqtt_client_t *client, const h2_pal_mqtt_event_t *event) {
    (void)client;
    events_t *events = (events_t *)user;
    if (event->type == H2_PAL_MQTT_EVENT_CONNECTED) {
        events->connected++;
    } else if (event->type == H2_PAL_MQTT_EVENT_PUBLISH_ACK) {
        events->publish_ack++;
    } else if (event->type == H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK) {
        events->subscribe_ack++;
        assert(event->data.subscribe_ack.result_count == 1u);
        assert(event->data.subscribe_ack.results[0] == H2_PAL_MQTT_SUBACK_QOS0);
    } else if (event->type == H2_PAL_MQTT_EVENT_UNSUBSCRIBE_ACK) {
        events->unsubscribe_ack++;
    } else if (event->type == H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED) {
        events->publish_received++;
        assert(event->data.publish_received.topic.len == strlen("topic/in"));
        assert(memcmp(event->data.publish_received.topic.data, "topic/in", strlen("topic/in")) == 0);
        assert(event->data.publish_received.payload.len == strlen("hello"));
        assert(memcmp(event->data.publish_received.payload.data, "hello", strlen("hello")) == 0);
    } else if (event->type == H2_PAL_MQTT_EVENT_DISCONNECTED) {
        events->disconnected++;
    }
}

static h2_pal_mqtt_client_config_t client_config(uint8_t *buffer, size_t buffer_len, events_t *events) {
    h2_pal_mqtt_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.endpoint.host.data = "localhost";
    config.endpoint.host.len = strlen("localhost");
    config.endpoint.port = 1883u;
    config.client_id.data = "client-test";
    config.client_id.len = strlen("client-test");
    config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
    config.keepalive_sec = 30u;
    config.connect_timeout_ms = 100u;
    config.operation_timeout_ms = 50u;
    config.clean_session = 1;
    config.network_buffer = buffer;
    config.network_buffer_len = buffer_len;
    config.on_event = on_event;
    config.event_user = events;
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
    assert(h2_coremqtt_create(&provider_config, &provider, &api) == H2_PAL_OK);

    uint8_t buffer[1024];
    events_t events;
    memset(&events, 0, sizeof(events));
    h2_pal_mqtt_client_config_t config = client_config(buffer, sizeof(buffer), &events);
    h2_pal_mqtt_client_t *client = NULL;
    assert(api.vtable->open(api.user, &config, &client) == H2_PAL_OK);
    assert(api.vtable->connect(api.user, client) == H2_PAL_OK);
    assert(events.connected == 1);

    h2_pal_mqtt_subscribe_item_t item = {
        .filter = { .data = "topic/in", .len = strlen("topic/in") },
        .qos = H2_PAL_MQTT_QOS0,
    };
    h2_pal_mqtt_subscribe_request_t sub = {
        .items = &item,
        .item_count = 1u,
        .timeout_ms = 50u,
    };
    uint16_t packet_id = 0u;
    assert(api.vtable->subscribe(api.user, client, &sub, &packet_id) == H2_PAL_OK);
    assert(packet_id != 0u);
    assert(api.vtable->process(api.user, client, 50u) == H2_PAL_OK);
    assert(events.subscribe_ack == 1);

    h2_pal_mqtt_publish_t publish;
    memset(&publish, 0, sizeof(publish));
    publish.topic.data = "topic/out";
    publish.topic.len = strlen("topic/out");
    publish.payload.data = (const uint8_t *)"payload";
    publish.payload.len = strlen("payload");
    publish.qos = H2_PAL_MQTT_QOS1;
    publish.timeout_ms = 50u;
    assert(api.vtable->publish(api.user, client, &publish, &packet_id) == H2_PAL_OK);
    assert(packet_id != 0u);
    assert(api.vtable->process(api.user, client, 50u) == H2_PAL_OK);
    assert(events.publish_ack == 1);

    fake_mqtt_platform_push_publish(&fake, "topic/in", (const uint8_t *)"hello", strlen("hello"));
    assert(api.vtable->process(api.user, client, 50u) == H2_PAL_OK);
    assert(events.publish_received == 1);

    h2_pal_mqtt_topic_filter_t filter = { .data = "topic/in", .len = strlen("topic/in") };
    h2_pal_mqtt_unsubscribe_request_t unsub = {
        .filters = &filter,
        .filter_count = 1u,
        .timeout_ms = 50u,
    };
    assert(api.vtable->unsubscribe(api.user, client, &unsub, &packet_id) == H2_PAL_OK);
    assert(api.vtable->process(api.user, client, 50u) == H2_PAL_OK);
    assert(events.unsubscribe_ack == 1);

    assert(api.vtable->disconnect(api.user, client, 50u) == H2_PAL_OK);
    assert(events.disconnected == 1);
    assert(api.vtable->connect(api.user, client) == H2_PAL_OK);
    assert(events.connected == 2);
    assert(api.vtable->disconnect(api.user, client, 50u) == H2_PAL_OK);

    api.vtable->close(api.user, client);
    h2_coremqtt_destroy(provider);
    return 0;
}
