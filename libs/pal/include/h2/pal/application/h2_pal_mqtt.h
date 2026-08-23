#ifndef H2_PAL_MQTT_H
#define H2_PAL_MQTT_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/net/h2_pal_net.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_mqtt_client h2_pal_mqtt_client_t;
typedef struct h2_pal_mqtt_event h2_pal_mqtt_event_t;
typedef struct h2_pal_mqtt_client_config h2_pal_mqtt_client_config_t;

typedef struct h2_pal_mqtt_str {
    const char *data;
    size_t len;
} h2_pal_mqtt_str_t;

typedef struct h2_pal_mqtt_bytes {
    const uint8_t *data;
    size_t len;
} h2_pal_mqtt_bytes_t;

typedef enum h2_pal_mqtt_qos {
    H2_PAL_MQTT_QOS0 = 0,
    H2_PAL_MQTT_QOS1 = 1,
} h2_pal_mqtt_qos_t;

typedef struct h2_pal_mqtt_topic_filter {
    const char *data;
    size_t len;
} h2_pal_mqtt_topic_filter_t;

typedef struct h2_pal_mqtt_publish {
    h2_pal_mqtt_str_t topic;
    h2_pal_mqtt_bytes_t payload;
    h2_pal_mqtt_qos_t qos;
    uint32_t timeout_ms;
    int retain;
} h2_pal_mqtt_publish_t;

typedef struct h2_pal_mqtt_subscribe_item {
    h2_pal_mqtt_topic_filter_t filter;
    h2_pal_mqtt_qos_t qos;
} h2_pal_mqtt_subscribe_item_t;

typedef struct h2_pal_mqtt_subscribe_request {
    const h2_pal_mqtt_subscribe_item_t *items;
    size_t item_count;
    uint32_t timeout_ms;
} h2_pal_mqtt_subscribe_request_t;

typedef struct h2_pal_mqtt_unsubscribe_request {
    const h2_pal_mqtt_topic_filter_t *filters;
    size_t filter_count;
    uint32_t timeout_ms;
} h2_pal_mqtt_unsubscribe_request_t;

typedef void (*h2_pal_mqtt_event_fn)(
    void *user,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_event_t *event);

typedef enum h2_pal_mqtt_transport {
    H2_PAL_MQTT_TRANSPORT_TCP = 0,
    H2_PAL_MQTT_TRANSPORT_TLS = 1,
} h2_pal_mqtt_transport_t;

typedef struct h2_pal_mqtt_endpoint {
    h2_pal_mqtt_str_t host;
    uint16_t port;
} h2_pal_mqtt_endpoint_t;

struct h2_pal_mqtt_client_config {
    h2_pal_mqtt_endpoint_t endpoint;
    h2_pal_mqtt_str_t client_id;
    h2_pal_mqtt_str_t username;
    h2_pal_mqtt_bytes_t password;
    h2_pal_mqtt_transport_t transport;
    const h2_pal_net_tls_config_t *tls;
    const h2_pal_net_bind_t *bind;
    uint16_t keepalive_sec;
    uint32_t connect_timeout_ms;
    uint32_t operation_timeout_ms;
    int clean_session;
    uint8_t *network_buffer;
    size_t network_buffer_len;
    h2_pal_mqtt_event_fn on_event;
    void *event_user;
};

typedef struct h2_pal_mqtt_vtable {
    h2_pal_result_t (*open)(
        void *user,
        const h2_pal_mqtt_client_config_t *config,
        h2_pal_mqtt_client_t **out_client);
    h2_pal_result_t (*connect)(void *user, h2_pal_mqtt_client_t *client);
    h2_pal_result_t (*disconnect)(void *user, h2_pal_mqtt_client_t *client, uint32_t timeout_ms);
    h2_pal_result_t (*publish)(
        void *user,
        h2_pal_mqtt_client_t *client,
        const h2_pal_mqtt_publish_t *message,
        uint16_t *out_packet_id);
    h2_pal_result_t (*subscribe)(
        void *user,
        h2_pal_mqtt_client_t *client,
        const h2_pal_mqtt_subscribe_request_t *request,
        uint16_t *out_packet_id);
    h2_pal_result_t (*unsubscribe)(
        void *user,
        h2_pal_mqtt_client_t *client,
        const h2_pal_mqtt_unsubscribe_request_t *request,
        uint16_t *out_packet_id);
    h2_pal_result_t (*process)(void *user, h2_pal_mqtt_client_t *client, uint32_t timeout_ms);
    void (*close)(void *user, h2_pal_mqtt_client_t *client);
} h2_pal_mqtt_vtable_t;

typedef struct h2_pal_mqtt_api {
    void *user;
    const h2_pal_mqtt_vtable_t *vtable;
} h2_pal_mqtt_api_t;

static inline h2_pal_result_t h2_pal_mqtt_open(
    const h2_pal_mqtt_api_t *api,
    const h2_pal_mqtt_client_config_t *config,
    h2_pal_mqtt_client_t **out_client) {
    if (config == NULL || out_client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->open == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->open(api->user, config, out_client);
}

static inline h2_pal_result_t h2_pal_mqtt_connect(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->connect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->connect(api->user, client);
}

static inline h2_pal_result_t h2_pal_mqtt_disconnect(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->disconnect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->disconnect(api->user, client, timeout_ms);
}

static inline h2_pal_result_t h2_pal_mqtt_publish(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_publish_t *message,
    uint16_t *out_packet_id) {
    if (client == NULL || message == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->publish == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->publish(api->user, client, message, out_packet_id);
}

static inline h2_pal_result_t h2_pal_mqtt_subscribe(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_subscribe_request_t *request,
    uint16_t *out_packet_id) {
    if (client == NULL || request == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->subscribe == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->subscribe(api->user, client, request, out_packet_id);
}

static inline h2_pal_result_t h2_pal_mqtt_unsubscribe(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_unsubscribe_request_t *request,
    uint16_t *out_packet_id) {
    if (client == NULL || request == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->unsubscribe == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->unsubscribe(api->user, client, request, out_packet_id);
}

static inline h2_pal_result_t h2_pal_mqtt_process(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->process == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->process(api->user, client, timeout_ms);
}

static inline void h2_pal_mqtt_close(
    const h2_pal_mqtt_api_t *api,
    h2_pal_mqtt_client_t *client) {
    if (api != NULL && api->vtable != NULL && api->vtable->close != NULL && client != NULL) {
        api->vtable->close(api->user, client);
    }
}

typedef enum h2_pal_mqtt_event_type {
    H2_PAL_MQTT_EVENT_CONNECTED = 1,
    H2_PAL_MQTT_EVENT_DISCONNECTED,
    H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED,
    H2_PAL_MQTT_EVENT_PUBLISH_ACK,
    H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK,
    H2_PAL_MQTT_EVENT_UNSUBSCRIBE_ACK,
    H2_PAL_MQTT_EVENT_ERROR,
} h2_pal_mqtt_event_type_t;

typedef enum h2_pal_mqtt_operation {
    H2_PAL_MQTT_OPERATION_OPEN = 1,
    H2_PAL_MQTT_OPERATION_CONNECT,
    H2_PAL_MQTT_OPERATION_DISCONNECT,
    H2_PAL_MQTT_OPERATION_PUBLISH,
    H2_PAL_MQTT_OPERATION_SUBSCRIBE,
    H2_PAL_MQTT_OPERATION_UNSUBSCRIBE,
    H2_PAL_MQTT_OPERATION_PROCESS,
    H2_PAL_MQTT_OPERATION_CLOSE,
} h2_pal_mqtt_operation_t;

typedef enum h2_pal_mqtt_disconnect_reason {
    H2_PAL_MQTT_DISCONNECT_REASON_LOCAL = 1,
    H2_PAL_MQTT_DISCONNECT_REASON_REMOTE,
    H2_PAL_MQTT_DISCONNECT_REASON_TRANSPORT_ERROR,
    H2_PAL_MQTT_DISCONNECT_REASON_PROTOCOL_ERROR,
    H2_PAL_MQTT_DISCONNECT_REASON_KEEPALIVE_TIMEOUT,
} h2_pal_mqtt_disconnect_reason_t;

typedef enum h2_pal_mqtt_suback_result {
    H2_PAL_MQTT_SUBACK_QOS0 = 0x00,
    H2_PAL_MQTT_SUBACK_QOS1 = 0x01,
    H2_PAL_MQTT_SUBACK_FAILURE = 0x80,
} h2_pal_mqtt_suback_result_t;

typedef struct h2_pal_mqtt_connected_event {
    int session_present;
} h2_pal_mqtt_connected_event_t;

typedef struct h2_pal_mqtt_disconnected_event {
    h2_pal_mqtt_disconnect_reason_t reason;
    h2_pal_result_t result;
} h2_pal_mqtt_disconnected_event_t;

typedef struct h2_pal_mqtt_publish_received_event {
    h2_pal_mqtt_str_t topic;
    h2_pal_mqtt_bytes_t payload;
    h2_pal_mqtt_qos_t qos;
    uint16_t packet_id;
    int retain;
    int duplicate;
} h2_pal_mqtt_publish_received_event_t;

typedef struct h2_pal_mqtt_publish_ack_event {
    uint16_t packet_id;
    h2_pal_result_t result;
} h2_pal_mqtt_publish_ack_event_t;

typedef struct h2_pal_mqtt_subscribe_ack_event {
    uint16_t packet_id;
    const h2_pal_mqtt_suback_result_t *results;
    size_t result_count;
    h2_pal_result_t result;
} h2_pal_mqtt_subscribe_ack_event_t;

typedef struct h2_pal_mqtt_unsubscribe_ack_event {
    uint16_t packet_id;
    h2_pal_result_t result;
} h2_pal_mqtt_unsubscribe_ack_event_t;

typedef struct h2_pal_mqtt_error_event {
    h2_pal_mqtt_operation_t operation;
    h2_pal_result_t result;
    int connected;
} h2_pal_mqtt_error_event_t;

struct h2_pal_mqtt_event {
    h2_pal_mqtt_event_type_t type;
    union {
        h2_pal_mqtt_connected_event_t connected;
        h2_pal_mqtt_disconnected_event_t disconnected;
        h2_pal_mqtt_publish_received_event_t publish_received;
        h2_pal_mqtt_publish_ack_event_t publish_ack;
        h2_pal_mqtt_subscribe_ack_event_t subscribe_ack;
        h2_pal_mqtt_unsubscribe_ack_event_t unsubscribe_ack;
        h2_pal_mqtt_error_event_t error;
    } data;
};

#ifdef __cplusplus
}
#endif

#endif
