#ifndef H2_COREMQTT_INTERNAL_H
#define H2_COREMQTT_INTERNAL_H

#include "h2_coremqtt.h"

#include "core_mqtt.h"

struct NetworkContext {
    struct h2_pal_mqtt_client *client;
};

struct h2_coremqtt {
    h2_coremqtt_config_t config;
    h2_pal_mqtt_api_t api;
};

typedef struct h2_coremqtt_client_records {
    MQTTPubAckInfo_t *outgoing;
    MQTTPubAckInfo_t *incoming;
    size_t outgoing_count;
    size_t incoming_count;
} h2_coremqtt_client_records_t;

struct h2_pal_mqtt_client {
    h2_coremqtt_t *provider;
    h2_pal_mqtt_client_config_t config;
    h2_pal_net_socket_t socket;
    h2_pal_net_socket_t tls_socket;
    int connected;
    int processing;
    int emitting;
    int close_requested;
    uint32_t recv_timeout_ms;
    const h2_pal_time_api_t *time_api;
    MQTTContext_t mqtt;
    MQTTFixedBuffer_t fixed_buffer;
    struct NetworkContext network;
    h2_coremqtt_client_records_t records;
};

h2_pal_result_t h2_coremqtt_client_open(
    h2_coremqtt_t *provider,
    const h2_pal_mqtt_client_config_t *config,
    h2_pal_mqtt_client_t **out_client);

void h2_coremqtt_client_close(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client);

h2_pal_result_t h2_coremqtt_client_connect(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client);
h2_pal_result_t h2_coremqtt_client_disconnect(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms);
h2_pal_result_t h2_coremqtt_client_publish(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_publish_t *message,
    uint16_t *out_packet_id);
h2_pal_result_t h2_coremqtt_client_subscribe(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_subscribe_request_t *request,
    uint16_t *out_packet_id);
h2_pal_result_t h2_coremqtt_client_unsubscribe(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_unsubscribe_request_t *request,
    uint16_t *out_packet_id);
h2_pal_result_t h2_coremqtt_client_process(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms);

h2_pal_result_t h2_coremqtt_status_to_result(MQTTStatus_t status);
MQTTQoS_t h2_coremqtt_qos_to_core(h2_pal_mqtt_qos_t qos);
void h2_coremqtt_emit_event(h2_pal_mqtt_client_t *client, const h2_pal_mqtt_event_t *event);

int32_t h2_coremqtt_transport_recv(NetworkContext_t *network, void *buffer, size_t bytes_to_recv);
int32_t h2_coremqtt_transport_send(NetworkContext_t *network, const void *buffer, size_t bytes_to_send);

#endif
