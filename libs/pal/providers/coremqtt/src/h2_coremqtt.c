#include "h2_coremqtt_internal.h"

#include <string.h>

static h2_pal_result_t mqtt_api_open(
    void *user,
    const h2_pal_mqtt_client_config_t *config,
    h2_pal_mqtt_client_t **out_client) {
    return h2_coremqtt_client_open((h2_coremqtt_t *)user, config, out_client);
}

static h2_pal_result_t mqtt_api_connect(void *user, h2_pal_mqtt_client_t *client) {
    return h2_coremqtt_client_connect((h2_coremqtt_t *)user, client);
}

static h2_pal_result_t mqtt_api_disconnect(void *user, h2_pal_mqtt_client_t *client, uint32_t timeout_ms) {
    return h2_coremqtt_client_disconnect((h2_coremqtt_t *)user, client, timeout_ms);
}

static h2_pal_result_t mqtt_api_publish(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_publish_t *message,
    uint16_t *out_packet_id) {
    return h2_coremqtt_client_publish((h2_coremqtt_t *)user, client, message, out_packet_id);
}

static h2_pal_result_t mqtt_api_subscribe(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_subscribe_request_t *request,
    uint16_t *out_packet_id) {
    return h2_coremqtt_client_subscribe((h2_coremqtt_t *)user, client, request, out_packet_id);
}

static h2_pal_result_t mqtt_api_unsubscribe(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_unsubscribe_request_t *request,
    uint16_t *out_packet_id) {
    return h2_coremqtt_client_unsubscribe((h2_coremqtt_t *)user, client, request, out_packet_id);
}

static h2_pal_result_t mqtt_api_process(void *user, h2_pal_mqtt_client_t *client, uint32_t timeout_ms) {
    return h2_coremqtt_client_process((h2_coremqtt_t *)user, client, timeout_ms);
}

static void mqtt_api_close(void *user, h2_pal_mqtt_client_t *client) {
    h2_coremqtt_client_close((h2_coremqtt_t *)user, client);
}

static const h2_pal_mqtt_vtable_t s_mqtt_vtable = {
    .open = mqtt_api_open,
    .connect = mqtt_api_connect,
    .disconnect = mqtt_api_disconnect,
    .publish = mqtt_api_publish,
    .subscribe = mqtt_api_subscribe,
    .unsubscribe = mqtt_api_unsubscribe,
    .process = mqtt_api_process,
    .close = mqtt_api_close,
};

h2_pal_result_t h2_coremqtt_status_to_result(MQTTStatus_t status) {
    switch (status) {
        case MQTTSuccess:
            return H2_PAL_OK;
        case MQTTBadParameter:
            return H2_PAL_ERR_INVALID_ARG;
        case MQTTNoMemory:
            return H2_PAL_ERR_NO_MEMORY;
        case MQTTNoDataAvailable:
        case MQTTNeedMoreBytes:
            return H2_PAL_ERR_TIMEOUT;
        case MQTTServerRefused:
        case MQTTBadResponse:
        case MQTTIllegalState:
        case MQTTStateCollision:
            return H2_PAL_ERR_INVALID_STATE;
        case MQTTKeepAliveTimeout:
            return H2_PAL_ERR_TIMEOUT;
        case MQTTSendFailed:
        case MQTTRecvFailed:
        default:
            return H2_PAL_ERR_IO;
    }
}

MQTTQoS_t h2_coremqtt_qos_to_core(h2_pal_mqtt_qos_t qos) {
    return qos == H2_PAL_MQTT_QOS1 ? MQTTQoS1 : MQTTQoS0;
}

void h2_coremqtt_emit_event(h2_pal_mqtt_client_t *client, const h2_pal_mqtt_event_t *event) {
    if (client != NULL && client->config.on_event != NULL && event != NULL) {
        client->emitting += 1;
        client->config.on_event(client->config.event_user, client, event);
        client->emitting -= 1;
    }
}

h2_pal_result_t h2_coremqtt_create(
    const h2_coremqtt_config_t *config,
    h2_coremqtt_t **out_mqtt,
    h2_pal_mqtt_api_t *out_api) {
    if (config == NULL || out_mqtt == NULL || out_api == NULL ||
        config->allocator == NULL || config->net == NULL || config->time == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_coremqtt_t *mqtt = (h2_coremqtt_t *)h2_pal_mem_alloc(config->allocator, sizeof(*mqtt));
    if (mqtt == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(mqtt, 0, sizeof(*mqtt));
    mqtt->config = *config;
    if (mqtt->config.outgoing_publish_records == 0u) {
        mqtt->config.outgoing_publish_records = 4u;
    }
    if (mqtt->config.incoming_publish_records == 0u) {
        mqtt->config.incoming_publish_records = 4u;
    }
    mqtt->api.user = mqtt;
    mqtt->api.vtable = &s_mqtt_vtable;
    *out_mqtt = mqtt;
    *out_api = mqtt->api;
    return H2_PAL_OK;
}

void h2_coremqtt_destroy(h2_coremqtt_t *mqtt) {
    if (mqtt == NULL) {
        return;
    }
    const h2_pal_mem_api_t *allocator = mqtt->config.allocator;
    h2_pal_mem_free(allocator, mqtt);
}
