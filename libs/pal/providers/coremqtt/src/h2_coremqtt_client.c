#include "h2_coremqtt_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define H2_COREMQTT_SUBACK_EVENT_MAX 16u

static _Thread_local const h2_pal_time_api_t *s_active_time_api;

static uint32_t coremqtt_now_ms(void) {
    uint64_t now = 0u;
    if (s_active_time_api == NULL || h2_pal_time_get_monotonic_ms(s_active_time_api, &now) != H2_PAL_OK) {
        return 0u;
    }
    return (uint32_t)now;
}

static void activate_client_time(const h2_pal_mqtt_client_t *client) {
    s_active_time_api = client == NULL ? NULL : client->time_api;
}

static h2_pal_result_t enter_client(h2_pal_mqtt_client_t *client) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (client->processing) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    client->processing = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t validate_str(h2_pal_mqtt_str_t value, int required) {
    if (value.len > (size_t)UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (required && (value.data == NULL || value.len == 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!required && value.data == NULL && value.len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t validate_bytes(h2_pal_mqtt_bytes_t value, int required) {
    if (value.len > (size_t)UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (required && (value.data == NULL || value.len == 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!required && value.data == NULL && value.len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t validate_config_strs(const h2_pal_mqtt_client_config_t *config) {
    h2_pal_result_t rc = validate_str(config->endpoint.host, 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = validate_str(config->client_id, 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = validate_str(config->username, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return validate_bytes(config->password, 0);
}

static h2_pal_result_t validate_qos(h2_pal_mqtt_qos_t qos) {
    if (qos == H2_PAL_MQTT_QOS0 || qos == H2_PAL_MQTT_QOS1) {
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static uint32_t timeout_or_default(uint32_t value, uint32_t fallback) {
    if (value != 0u) {
        return value;
    }
    return fallback == 0u ? 1000u : fallback;
}

static void emit_error(h2_pal_mqtt_client_t *client, h2_pal_mqtt_operation_t op, h2_pal_result_t result) {
    h2_pal_mqtt_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = H2_PAL_MQTT_EVENT_ERROR;
    event.data.error.operation = op;
    event.data.error.result = result;
    event.data.error.connected = client == NULL ? 0 : client->connected;
    h2_coremqtt_emit_event(client, &event);
}

static void emit_disconnected(
    h2_pal_mqtt_client_t *client,
    h2_pal_mqtt_disconnect_reason_t reason,
    h2_pal_result_t result) {
    h2_pal_mqtt_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = H2_PAL_MQTT_EVENT_DISCONNECTED;
    event.data.disconnected.reason = reason;
    event.data.disconnected.result = result;
    h2_coremqtt_emit_event(client, &event);
}

static void close_client_socket(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client) {
    if (provider == NULL || client == NULL) {
        return;
    }
    if (client->tls_socket >= 0 && client->tls_socket != client->socket) {
        h2_pal_net_close(provider->config.net, client->tls_socket);
    }
    if (client->socket >= 0) {
        h2_pal_net_close(provider->config.net, client->socket);
    }
    client->socket = -1;
    client->tls_socket = -1;
}

static void free_client_storage(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client) {
    if (provider == NULL || client == NULL) {
        return;
    }
    const h2_pal_mem_api_t *allocator = provider->config.allocator;
    h2_pal_mem_free(allocator, client->records.outgoing);
    h2_pal_mem_free(allocator, client->records.incoming);
    h2_pal_mem_free(allocator, client);
}

static void leave_client(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client) {
    if (client == NULL) {
        return;
    }
    client->processing = 0;
    if (client->close_requested) {
        free_client_storage(provider, client);
    }
}

static void coremqtt_event_callback(
    MQTTContext_t *context,
    MQTTPacketInfo_t *packet_info,
    MQTTDeserializedInfo_t *deserialized_info) {
    if (context == NULL || packet_info == NULL || deserialized_info == NULL) {
        return;
    }
    NetworkContext_t *network = context->transportInterface.pNetworkContext;
    h2_pal_mqtt_client_t *client = network == NULL ? NULL : network->client;
    if (client == NULL) {
        return;
    }

    h2_pal_mqtt_event_t event;
    memset(&event, 0, sizeof(event));
    uint8_t packet_type = packet_info->type & 0xF0u;
    if (packet_type == MQTT_PACKET_TYPE_PUBLISH && deserialized_info->pPublishInfo != NULL) {
        MQTTPublishInfo_t *publish = deserialized_info->pPublishInfo;
        if (publish->qos > MQTTQoS1) {
            emit_error(client, H2_PAL_MQTT_OPERATION_PROCESS, H2_PAL_ERR_UNSUPPORTED);
            return;
        }
        event.type = H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED;
        event.data.publish_received.topic.data = publish->pTopicName;
        event.data.publish_received.topic.len = publish->topicNameLength;
        event.data.publish_received.payload.data = (const uint8_t *)publish->pPayload;
        event.data.publish_received.payload.len = publish->payloadLength;
        event.data.publish_received.qos = publish->qos == MQTTQoS1 ? H2_PAL_MQTT_QOS1 : H2_PAL_MQTT_QOS0;
        event.data.publish_received.packet_id = deserialized_info->packetIdentifier;
        event.data.publish_received.retain = publish->retain ? 1 : 0;
        event.data.publish_received.duplicate = publish->dup ? 1 : 0;
        h2_coremqtt_emit_event(client, &event);
        return;
    }
    if (packet_type == MQTT_PACKET_TYPE_PUBACK) {
        event.type = H2_PAL_MQTT_EVENT_PUBLISH_ACK;
        event.data.publish_ack.packet_id = deserialized_info->packetIdentifier;
        event.data.publish_ack.result = h2_coremqtt_status_to_result(deserialized_info->deserializationResult);
        h2_coremqtt_emit_event(client, &event);
        return;
    }
    if (packet_type == MQTT_PACKET_TYPE_SUBACK) {
        uint8_t *codes = NULL;
        size_t code_count = 0u;
        MQTTStatus_t status = MQTT_GetSubAckStatusCodes(packet_info, &codes, &code_count);
        h2_pal_mqtt_suback_result_t results[H2_COREMQTT_SUBACK_EVENT_MAX];
        size_t result_count = 0u;
        h2_pal_result_t result = h2_coremqtt_status_to_result(status);
        if (status == MQTTSuccess && code_count <= H2_COREMQTT_SUBACK_EVENT_MAX) {
            result_count = code_count;
            for (size_t i = 0; i < code_count; ++i) {
                if (codes[i] == MQTTSubAckSuccessQos0) {
                    results[i] = H2_PAL_MQTT_SUBACK_QOS0;
                } else if (codes[i] == MQTTSubAckSuccessQos1) {
                    results[i] = H2_PAL_MQTT_SUBACK_QOS1;
                } else {
                    results[i] = H2_PAL_MQTT_SUBACK_FAILURE;
                }
            }
        } else if (status == MQTTSuccess) {
            result = H2_PAL_ERR_NO_SPACE;
        }
        event.type = H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK;
        event.data.subscribe_ack.packet_id = deserialized_info->packetIdentifier;
        event.data.subscribe_ack.results = result_count == 0u ? NULL : results;
        event.data.subscribe_ack.result_count = result_count;
        event.data.subscribe_ack.result = result;
        h2_coremqtt_emit_event(client, &event);
        return;
    }
    if (packet_type == MQTT_PACKET_TYPE_UNSUBACK) {
        event.type = H2_PAL_MQTT_EVENT_UNSUBSCRIBE_ACK;
        event.data.unsubscribe_ack.packet_id = deserialized_info->packetIdentifier;
        event.data.unsubscribe_ack.result = h2_coremqtt_status_to_result(deserialized_info->deserializationResult);
        h2_coremqtt_emit_event(client, &event);
    }
}

static h2_pal_result_t init_core_context(h2_pal_mqtt_client_t *client) {
    TransportInterface_t transport;
    memset(&transport, 0, sizeof(transport));
    transport.recv = h2_coremqtt_transport_recv;
    transport.send = h2_coremqtt_transport_send;
    transport.pNetworkContext = &client->network;
    client->fixed_buffer.pBuffer = client->config.network_buffer;
    client->fixed_buffer.size = client->config.network_buffer_len;
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Init(&client->mqtt, &transport, coremqtt_now_ms, coremqtt_event_callback, &client->fixed_buffer);
    if (status != MQTTSuccess) {
        return h2_coremqtt_status_to_result(status);
    }
    status = MQTT_InitStatefulQoS(
        &client->mqtt,
        client->records.outgoing,
        client->records.outgoing_count,
        client->records.incoming,
        client->records.incoming_count);
    return h2_coremqtt_status_to_result(status);
}

h2_pal_result_t h2_coremqtt_client_open(
    h2_coremqtt_t *provider,
    const h2_pal_mqtt_client_config_t *config,
    h2_pal_mqtt_client_t **out_client) {
    if (provider == NULL || config == NULL || out_client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_client = NULL;
    h2_pal_result_t rc = validate_config_strs(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (config->network_buffer == NULL || config->network_buffer_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->transport == H2_PAL_MQTT_TRANSPORT_TLS && config->tls == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->transport != H2_PAL_MQTT_TRANSPORT_TCP && config->transport != H2_PAL_MQTT_TRANSPORT_TLS) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (provider->config.outgoing_publish_records > SIZE_MAX / sizeof(MQTTPubAckInfo_t) ||
        provider->config.incoming_publish_records > SIZE_MAX / sizeof(MQTTPubAckInfo_t)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    const h2_pal_mem_api_t *allocator = provider->config.allocator;
    h2_pal_mqtt_client_t *client = (h2_pal_mqtt_client_t *)h2_pal_mem_alloc(allocator, sizeof(*client));
    if (client == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(client, 0, sizeof(*client));
    client->provider = provider;
    client->config = *config;
    client->socket = -1;
    client->tls_socket = -1;
    client->network.client = client;
    client->time_api = provider->config.time;
    client->recv_timeout_ms = timeout_or_default(config->operation_timeout_ms, 1000u);
    client->records.outgoing_count = provider->config.outgoing_publish_records;
    client->records.incoming_count = provider->config.incoming_publish_records;
    client->records.outgoing = (MQTTPubAckInfo_t *)h2_pal_mem_alloc(
        allocator,
        client->records.outgoing_count * sizeof(client->records.outgoing[0]));
    client->records.incoming = (MQTTPubAckInfo_t *)h2_pal_mem_alloc(
        allocator,
        client->records.incoming_count * sizeof(client->records.incoming[0]));
    if (client->records.outgoing == NULL || client->records.incoming == NULL) {
        h2_coremqtt_client_close(provider, client);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(client->records.outgoing, 0, client->records.outgoing_count * sizeof(client->records.outgoing[0]));
    memset(client->records.incoming, 0, client->records.incoming_count * sizeof(client->records.incoming[0]));
    rc = init_core_context(client);
    if (rc != H2_PAL_OK) {
        h2_coremqtt_client_close(provider, client);
        return rc;
    }
    *out_client = client;
    return H2_PAL_OK;
}

void h2_coremqtt_client_close(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client) {
    if (provider == NULL || client == NULL) {
        return;
    }
    close_client_socket(provider, client);
    client->connected = 0;
    if (client->processing || client->emitting) {
        client->close_requested = 1;
        return;
    }
    free_client_storage(provider, client);
}

h2_pal_result_t h2_coremqtt_client_connect(h2_coremqtt_t *provider, h2_pal_mqtt_client_t *client) {
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (client->connected || client->socket >= 0) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    memset(client->records.outgoing, 0, client->records.outgoing_count * sizeof(client->records.outgoing[0]));
    memset(client->records.incoming, 0, client->records.incoming_count * sizeof(client->records.incoming[0]));
    rc = init_core_context(client);
    if (rc != H2_PAL_OK) {
        leave_client(provider, client);
        return rc;
    }

    const h2_pal_net_api_t *net = provider->config.net;
    h2_pal_net_addr_t addr;
    const h2_pal_mem_api_t *allocator = provider->config.allocator;
    char *host = (char *)h2_pal_mem_alloc(allocator, client->config.endpoint.host.len + 1u);
    if (host == NULL) {
        leave_client(provider, client);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(host, client->config.endpoint.host.data, client->config.endpoint.host.len);
    host[client->config.endpoint.host.len] = '\0';
    rc = h2_pal_net_resolve_addr(net, host, &addr);
    h2_pal_mem_free(allocator, host);
    if (rc == H2_PAL_OK) {
        addr.port = client->config.endpoint.port;
        rc = h2_pal_net_tcp_open_bound(net, addr.family, client->config.bind, &client->socket);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_net_tcp_connect(
            net,
            client->socket,
            &addr,
            timeout_or_default(client->config.connect_timeout_ms, client->config.operation_timeout_ms));
    }
    if (rc == H2_PAL_OK && client->config.transport == H2_PAL_MQTT_TRANSPORT_TLS) {
        rc = h2_pal_net_tls_wrap(
            net,
            client->socket,
            client->config.tls,
            timeout_or_default(client->config.connect_timeout_ms, client->config.operation_timeout_ms),
            &client->tls_socket);
    }
    if (rc != H2_PAL_OK) {
        if (client->socket >= 0) {
            close_client_socket(provider, client);
        }
        emit_error(client, H2_PAL_MQTT_OPERATION_CONNECT, rc);
        leave_client(provider, client);
        return rc;
    }

    MQTTConnectInfo_t info;
    memset(&info, 0, sizeof(info));
    info.cleanSession = client->config.clean_session ? true : false;
    info.keepAliveSeconds = client->config.keepalive_sec;
    info.pClientIdentifier = client->config.client_id.data;
    info.clientIdentifierLength = (uint16_t)client->config.client_id.len;
    info.pUserName = client->config.username.data;
    info.userNameLength = (uint16_t)client->config.username.len;
    info.pPassword = (const char *)client->config.password.data;
    info.passwordLength = (uint16_t)client->config.password.len;

    bool session_present = false;
    client->recv_timeout_ms = timeout_or_default(client->config.operation_timeout_ms, client->config.connect_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Connect(
        &client->mqtt,
        &info,
        NULL,
        timeout_or_default(client->config.connect_timeout_ms, client->config.operation_timeout_ms),
        &session_present);
    rc = h2_coremqtt_status_to_result(status);
    if (rc != H2_PAL_OK) {
        close_client_socket(provider, client);
        emit_error(client, H2_PAL_MQTT_OPERATION_CONNECT, rc);
        leave_client(provider, client);
        return rc;
    }
    client->connected = 1;
    h2_pal_mqtt_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = H2_PAL_MQTT_EVENT_CONNECTED;
    event.data.connected.session_present = session_present ? 1 : 0;
    h2_coremqtt_emit_event(client, &event);
    leave_client(provider, client);
    return H2_PAL_OK;
}

h2_pal_result_t h2_coremqtt_client_disconnect(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms) {
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!client->connected || client->socket < 0) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    client->recv_timeout_ms = timeout_or_default(timeout_ms, client->config.operation_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Disconnect(&client->mqtt);
    rc = h2_coremqtt_status_to_result(status);
    close_client_socket(provider, client);
    client->connected = 0;
    emit_disconnected(client, H2_PAL_MQTT_DISCONNECT_REASON_LOCAL, rc);
    leave_client(provider, client);
    return rc;
}

h2_pal_result_t h2_coremqtt_client_publish(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_publish_t *message,
    uint16_t *out_packet_id) {
    if (out_packet_id != NULL) {
        *out_packet_id = 0u;
    }
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider || message == NULL ||
        (message->payload.data == NULL && message->payload.len != 0u)) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = validate_qos(message->qos);
    if (rc == H2_PAL_OK) {
        rc = validate_str(message->topic, 1);
    }
    if (rc != H2_PAL_OK) {
        leave_client(provider, client);
        return rc;
    }
    if (!client->connected) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    MQTTPublishInfo_t info;
    memset(&info, 0, sizeof(info));
    info.qos = h2_coremqtt_qos_to_core(message->qos);
    info.retain = message->retain ? true : false;
    info.pTopicName = message->topic.data;
    info.topicNameLength = (uint16_t)message->topic.len;
    info.pPayload = message->payload.data;
    info.payloadLength = message->payload.len;
    uint16_t packet_id = message->qos == H2_PAL_MQTT_QOS0 ? 0u : MQTT_GetPacketId(&client->mqtt);
    client->recv_timeout_ms = timeout_or_default(message->timeout_ms, client->config.operation_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Publish(&client->mqtt, &info, packet_id);
    rc = h2_coremqtt_status_to_result(status);
    if (out_packet_id != NULL && rc == H2_PAL_OK) {
        *out_packet_id = packet_id;
    }
    if (rc != H2_PAL_OK) {
        emit_error(client, H2_PAL_MQTT_OPERATION_PUBLISH, rc);
    }
    leave_client(provider, client);
    return rc;
}

static h2_pal_result_t fill_subscribe_infos(
    const h2_pal_mem_api_t *allocator,
    const h2_pal_mqtt_subscribe_item_t *items,
    size_t item_count,
    MQTTSubscribeInfo_t **out_infos) {
    if (items == NULL || item_count == 0u || out_infos == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (item_count > SIZE_MAX / sizeof(MQTTSubscribeInfo_t)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    MQTTSubscribeInfo_t *infos = (MQTTSubscribeInfo_t *)h2_pal_mem_alloc(allocator, item_count * sizeof(infos[0]));
    if (infos == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(infos, 0, item_count * sizeof(infos[0]));
    for (size_t i = 0; i < item_count; ++i) {
        h2_pal_result_t rc = validate_qos(items[i].qos);
        if (rc == H2_PAL_OK) {
            h2_pal_mqtt_str_t filter = { .data = items[i].filter.data, .len = items[i].filter.len };
            rc = validate_str(filter, 1);
        }
        if (rc != H2_PAL_OK) {
            h2_pal_mem_free(allocator, infos);
            return rc;
        }
        infos[i].qos = h2_coremqtt_qos_to_core(items[i].qos);
        infos[i].pTopicFilter = items[i].filter.data;
        infos[i].topicFilterLength = (uint16_t)items[i].filter.len;
    }
    *out_infos = infos;
    return H2_PAL_OK;
}

h2_pal_result_t h2_coremqtt_client_subscribe(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_subscribe_request_t *request,
    uint16_t *out_packet_id) {
    if (out_packet_id != NULL) {
        *out_packet_id = 0u;
    }
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider || request == NULL) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!client->connected) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    MQTTSubscribeInfo_t *infos = NULL;
    rc = fill_subscribe_infos(provider->config.allocator, request->items, request->item_count, &infos);
    if (rc != H2_PAL_OK) {
        leave_client(provider, client);
        return rc;
    }
    uint16_t packet_id = MQTT_GetPacketId(&client->mqtt);
    client->recv_timeout_ms = timeout_or_default(request->timeout_ms, client->config.operation_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Subscribe(&client->mqtt, infos, request->item_count, packet_id);
    h2_pal_mem_free(provider->config.allocator, infos);
    rc = h2_coremqtt_status_to_result(status);
    if (out_packet_id != NULL && rc == H2_PAL_OK) {
        *out_packet_id = packet_id;
    }
    if (rc != H2_PAL_OK) {
        emit_error(client, H2_PAL_MQTT_OPERATION_SUBSCRIBE, rc);
    }
    leave_client(provider, client);
    return rc;
}

h2_pal_result_t h2_coremqtt_client_unsubscribe(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    const h2_pal_mqtt_unsubscribe_request_t *request,
    uint16_t *out_packet_id) {
    if (out_packet_id != NULL) {
        *out_packet_id = 0u;
    }
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider || request == NULL ||
        request->filters == NULL || request->filter_count == 0u) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!client->connected) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (request->filter_count > SIZE_MAX / sizeof(MQTTSubscribeInfo_t)) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    MQTTSubscribeInfo_t *infos =
        (MQTTSubscribeInfo_t *)h2_pal_mem_alloc(provider->config.allocator, request->filter_count * sizeof(infos[0]));
    if (infos == NULL) {
        leave_client(provider, client);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(infos, 0, request->filter_count * sizeof(infos[0]));
    for (size_t i = 0; i < request->filter_count; ++i) {
        h2_pal_mqtt_str_t filter = { .data = request->filters[i].data, .len = request->filters[i].len };
        rc = validate_str(filter, 1);
        if (rc != H2_PAL_OK) {
            h2_pal_mem_free(provider->config.allocator, infos);
            leave_client(provider, client);
            return rc;
        }
        infos[i].pTopicFilter = request->filters[i].data;
        infos[i].topicFilterLength = (uint16_t)request->filters[i].len;
    }
    uint16_t packet_id = MQTT_GetPacketId(&client->mqtt);
    client->recv_timeout_ms = timeout_or_default(request->timeout_ms, client->config.operation_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_Unsubscribe(&client->mqtt, infos, request->filter_count, packet_id);
    h2_pal_mem_free(provider->config.allocator, infos);
    rc = h2_coremqtt_status_to_result(status);
    if (out_packet_id != NULL && rc == H2_PAL_OK) {
        *out_packet_id = packet_id;
    }
    if (rc != H2_PAL_OK) {
        emit_error(client, H2_PAL_MQTT_OPERATION_UNSUBSCRIBE, rc);
    }
    leave_client(provider, client);
    return rc;
}

h2_pal_result_t h2_coremqtt_client_process(
    h2_coremqtt_t *provider,
    h2_pal_mqtt_client_t *client,
    uint32_t timeout_ms) {
    h2_pal_result_t rc = enter_client(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (provider == NULL || client->provider != provider) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!client->connected) {
        leave_client(provider, client);
        return H2_PAL_ERR_INVALID_STATE;
    }
    client->recv_timeout_ms = timeout_or_default(timeout_ms, client->config.operation_timeout_ms);
    activate_client_time(client);
    MQTTStatus_t status = MQTT_ProcessLoop(&client->mqtt);
    rc = h2_coremqtt_status_to_result(status);
    if (status == MQTTKeepAliveTimeout) {
        client->connected = 0;
        close_client_socket(provider, client);
        emit_disconnected(client, H2_PAL_MQTT_DISCONNECT_REASON_KEEPALIVE_TIMEOUT, rc);
    } else if (status == MQTTRecvFailed || status == MQTTSendFailed) {
        client->connected = 0;
        close_client_socket(provider, client);
        emit_disconnected(client, H2_PAL_MQTT_DISCONNECT_REASON_TRANSPORT_ERROR, rc);
    } else if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) {
        client->connected = 0;
        close_client_socket(provider, client);
        emit_disconnected(client, H2_PAL_MQTT_DISCONNECT_REASON_PROTOCOL_ERROR, rc);
    }
    leave_client(provider, client);
    return rc;
}
