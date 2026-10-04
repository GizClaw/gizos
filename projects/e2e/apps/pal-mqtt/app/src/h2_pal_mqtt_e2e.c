#include "h2_pal_mqtt_e2e.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { result->line = __LINE__; \
    result->detail = rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc; goto done; } } while (0)

static const char *const case_ids[] = {
#define H2_PAL_MQTT_CASE(symbol, id) id,
#include "h2_pal_mqtt_cases.inc"
#undef H2_PAL_MQTT_CASE
};

typedef struct scratch {
    uint8_t network[8192];
    uint8_t payload[4097];
    uint8_t expected[4097];
} scratch_t;

typedef struct events {
    const h2_pal_mqtt_api_t *api;
    h2_pal_mqtt_client_t **owned_client;
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    unsigned connected;
    unsigned received;
    unsigned disconnected;
    unsigned subacks;
    unsigned pubacks;
    unsigned unsubacks;
    unsigned errors;
    int invalid;
    int close_on_publish;
    int closed_in_callback;
    int session_present;
    int error_connected;
    int retain;
    h2_pal_mqtt_qos_t qos;
    uint16_t sub_id;
    uint16_t pub_id;
    uint16_t unsub_id;
    size_t sub_count;
    h2_pal_mqtt_suback_result_t sub_results[2];
    h2_pal_mqtt_disconnect_reason_t reason;
    h2_pal_result_t disconnect_result;
    h2_pal_result_t error_result;
    h2_pal_mqtt_operation_t error_operation;
} events_t;

static void event_callback(void *user, h2_pal_mqtt_client_t *client,
                           const h2_pal_mqtt_event_t *event) {
    events_t *state = user;
    if (state->closed_in_callback || client != *state->owned_client) state->invalid = 1;
    switch (event->type) {
        case H2_PAL_MQTT_EVENT_CONNECTED:
            ++state->connected;
            state->session_present = event->data.connected.session_present;
            break;
        case H2_PAL_MQTT_EVENT_DISCONNECTED:
            ++state->disconnected;
            state->reason = event->data.disconnected.reason;
            state->disconnect_result = event->data.disconnected.result;
            break;
        case H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK: {
            const h2_pal_mqtt_subscribe_ack_event_t *ack = &event->data.subscribe_ack;
            ++state->subacks;
            state->sub_id = ack->packet_id;
            state->sub_count = ack->result_count;
            if (ack->result != H2_PAL_OK || ack->results == NULL ||
                ack->result_count == 0u || ack->result_count > 2u) state->invalid = 1;
            else memcpy(state->sub_results, ack->results, ack->result_count * sizeof(ack->results[0]));
            break;
        }
        case H2_PAL_MQTT_EVENT_PUBLISH_ACK:
            ++state->pubacks;
            state->pub_id = event->data.publish_ack.packet_id;
            if (event->data.publish_ack.result != H2_PAL_OK) state->invalid = 1;
            break;
        case H2_PAL_MQTT_EVENT_UNSUBSCRIBE_ACK:
            ++state->unsubacks;
            state->unsub_id = event->data.unsubscribe_ack.packet_id;
            if (event->data.unsubscribe_ack.result != H2_PAL_OK) state->invalid = 1;
            break;
        case H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED: {
            const h2_pal_mqtt_publish_received_event_t *publish = &event->data.publish_received;
            ++state->received;
            if (publish->topic.data == NULL || publish->topic.len != strlen(state->topic) ||
                memcmp(publish->topic.data, state->topic, publish->topic.len) != 0 ||
                publish->payload.len != state->payload_len ||
                (publish->payload.len != 0u && (publish->payload.data == NULL ||
                 memcmp(publish->payload.data, state->payload, publish->payload.len) != 0))) state->invalid = 1;
            state->retain = publish->retain;
            state->qos = publish->qos;
            if (publish->qos == H2_PAL_MQTT_QOS1 && publish->packet_id == 0u) state->invalid = 1;
            if (state->close_on_publish) {
                h2_pal_mqtt_close(state->api, client);
                *state->owned_client = NULL;
                state->closed_in_callback = 1;
            }
            break;
        }
        case H2_PAL_MQTT_EVENT_ERROR:
            ++state->errors;
            state->error_result = event->data.error.result;
            state->error_operation = event->data.error.operation;
            state->error_connected = event->data.error.connected;
            break;
        default:
            state->invalid = 1;
            break;
    }
}

static uint64_t now(const h2_pal_mqtt_e2e_config_t *config) {
    uint64_t value = 0u;
    (void)h2_pal_time_get_monotonic_ms(config->runtime->time, &value);
    return value;
}

static int wait_for(const h2_pal_mqtt_e2e_config_t *config, h2_pal_mqtt_client_t *client,
                    const unsigned *counter, unsigned wanted, uint32_t budget) {
    uint64_t start = 0u;
    int time_rc = h2_pal_time_get_monotonic_ms(config->runtime->time, &start);
    if (time_rc != H2_PAL_OK) return time_rc;
    while (*counter < wanted) {
        uint64_t current = 0u;
        time_rc = h2_pal_time_get_monotonic_ms(config->runtime->time, &current);
        if (time_rc != H2_PAL_OK) return time_rc;
        if (current - start >= budget) return H2_PAL_ERR_TIMEOUT;
        int rc = h2_pal_mqtt_process(config->runtime->mqtt, client, 20u);
        if (*counter >= wanted) return H2_PAL_OK;
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) return rc;
    }
    return H2_PAL_OK;
}

static int subscribe(const h2_pal_mqtt_e2e_config_t *config, h2_pal_mqtt_client_t *client,
                     events_t *state, const char *topic, h2_pal_mqtt_qos_t qos) {
    h2_pal_mqtt_subscribe_item_t item = {.filter = {topic, strlen(topic)}, .qos = qos};
    h2_pal_mqtt_subscribe_request_t request = {.items = &item, .item_count = 1u};
    uint16_t packet_id = 0u;
    unsigned wanted = state->subacks + 1u;
    int rc = h2_pal_mqtt_subscribe(config->runtime->mqtt, client, &request, &packet_id);
    if (rc == H2_PAL_OK) rc = wait_for(config, client, &state->subacks, wanted, config->timeout_ms);
    if (rc == H2_PAL_OK && (state->invalid || packet_id == 0u || state->sub_id != packet_id ||
        state->sub_count != 1u || state->sub_results[0] != (h2_pal_mqtt_suback_result_t)qos)) rc = H2_PAL_ERR_IO;
    return rc;
}

static void run_case(const h2_pal_mqtt_e2e_config_t *config, h2_pal_mqtt_e2e_case_t which,
                     scratch_t *scratch, h2_pal_mqtt_e2e_case_result_t *result) {
    const h2_pal_mqtt_api_t *api = config->runtime->mqtt;
    h2_pal_mqtt_client_t *client = NULL;
    char client_id[192], topic[256] = {0}, other_topic[256];
    events_t state = {.api = api, .owned_client = &client, .topic = topic,
                      .payload = scratch->expected, .payload_len = 17u};
    int connected = 0;
    int count = snprintf(client_id, sizeof(client_id), "%s-%s", config->session, result->id);
    int rc = H2_PAL_OK;
    CHECK(count > 0 && (size_t)count < sizeof(client_id));
    count = snprintf(topic, sizeof(topic), "%s/%s/%s", config->topic_prefix, config->session, result->id);
    CHECK(count > 0 && (size_t)count < sizeof(topic));
    count = snprintf(other_topic, sizeof(other_topic), "%s/other", topic);
    CHECK(count > 0 && (size_t)count < sizeof(other_topic));
    for (size_t i = 0u; i < sizeof(scratch->payload); ++i) scratch->payload[i] = (uint8_t)(i * 37u + 11u);
    memcpy(scratch->expected, scratch->payload, sizeof(scratch->expected));
    h2_pal_mqtt_client_config_t client_config = {
        .endpoint = {{config->host, strlen(config->host)}, config->tcp_port},
        .client_id = {client_id, strlen(client_id)}, .clean_session = 1,
        .keepalive_sec = 30u, .connect_timeout_ms = config->timeout_ms,
        .operation_timeout_ms = 20u, .network_buffer = scratch->network,
        .network_buffer_len = sizeof(scratch->network), .on_event = event_callback, .event_user = &state,
    };
    h2_pal_mqtt_publish_t message = {.topic = {topic, strlen(topic)},
        .payload = {scratch->payload, state.payload_len}, .qos = H2_PAL_MQTT_QOS0};
    h2_pal_mqtt_subscribe_item_t item = {.filter = {topic, strlen(topic)}, .qos = H2_PAL_MQTT_QOS0};
    h2_pal_mqtt_subscribe_request_t request = {.items = &item, .item_count = 1u};
    h2_pal_mqtt_topic_filter_t filter = {topic, strlen(topic)};
    h2_pal_mqtt_unsubscribe_request_t unsubscribe_request = {.filters = &filter, .filter_count = 1u};
    uint16_t packet_id = 0u;
    if ((which >= H2_PAL_MQTT_E2E_TLS_TRUSTED && which <= H2_PAL_MQTT_E2E_TLS_WRONG_NAME) ||
        (config->smoke_only && config->smoke_transport == H2_PAL_MQTT_TRANSPORT_TLS)) {
        client_config.transport = H2_PAL_MQTT_TRANSPORT_TLS;
        client_config.endpoint.port = config->tls_port;
        client_config.tls = which == H2_PAL_MQTT_E2E_TLS_UNTRUSTED ? config->untrusted_tls :
            which == H2_PAL_MQTT_E2E_TLS_WRONG_NAME ? config->wrong_name_tls : config->trusted_tls;
    }
    if (which == H2_PAL_MQTT_E2E_AUTHENTICATED || which == H2_PAL_MQTT_E2E_AUTH_REFUSED) {
        client_config.username = (h2_pal_mqtt_str_t){"fixture", 7u};
        const char *password = which == H2_PAL_MQTT_E2E_AUTH_REFUSED ? "wrong" : "fixture-password";
        client_config.password = (h2_pal_mqtt_bytes_t){(const uint8_t *)password, strlen(password)};
    }
    if (which == H2_PAL_MQTT_E2E_CONNECT_TIMEOUT) client_config.connect_timeout_ms = 200u;
    if (which == H2_PAL_MQTT_E2E_KEEPALIVE_TIMEOUT) client_config.keepalive_sec = 1u;
    if (which == H2_PAL_MQTT_E2E_INVALID_OPEN_HOST) client_config.endpoint.host.len = 0u;
    if (which == H2_PAL_MQTT_E2E_INVALID_OPEN_BUFFER) client_config.network_buffer = NULL;
    if (which == H2_PAL_MQTT_E2E_INVALID_OPEN_TLS) {
        client_config.transport = H2_PAL_MQTT_TRANSPORT_TLS;
        client_config.tls = NULL;
    }
    rc = h2_pal_mqtt_open(api, &client_config, &client);
    if (which >= H2_PAL_MQTT_E2E_INVALID_OPEN_HOST && which <= H2_PAL_MQTT_E2E_INVALID_OPEN_TLS) {
        CHECK(rc == H2_PAL_ERR_INVALID_ARG && client == NULL && state.connected == 0u);
        result->passed = 1;
        goto done;
    }
    CHECK(rc == H2_PAL_OK && client != NULL);
    if (which == H2_PAL_MQTT_E2E_OPEN_CLOSE) { result->passed = 1; goto done; }
    if (which == H2_PAL_MQTT_E2E_DISCONNECTED_OPERATIONS) {
        CHECK(h2_pal_mqtt_publish(api, client, &message, &packet_id) == H2_PAL_ERR_INVALID_STATE);
        CHECK(h2_pal_mqtt_subscribe(api, client, &request, &packet_id) == H2_PAL_ERR_INVALID_STATE);
        CHECK(h2_pal_mqtt_unsubscribe(api, client, &unsubscribe_request, &packet_id) == H2_PAL_ERR_INVALID_STATE);
        CHECK(h2_pal_mqtt_process(api, client, 20u) == H2_PAL_ERR_INVALID_STATE);
        CHECK(h2_pal_mqtt_disconnect(api, client, 20u) == H2_PAL_ERR_INVALID_STATE);
        CHECK(state.connected == 0u && state.errors == 0u);
        result->passed = 1;
        goto done;
    }
    rc = h2_pal_mqtt_connect(api, client);
    if (which == H2_PAL_MQTT_E2E_AUTH_REFUSED || which == H2_PAL_MQTT_E2E_CONNECT_TIMEOUT ||
        which == H2_PAL_MQTT_E2E_TLS_UNTRUSTED || which == H2_PAL_MQTT_E2E_TLS_WRONG_NAME) {
        int expected = which == H2_PAL_MQTT_E2E_AUTH_REFUSED ? H2_PAL_ERR_INVALID_STATE :
            which == H2_PAL_MQTT_E2E_CONNECT_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_TLS_VERIFY;
        CHECK(rc == expected && state.errors == 1u && state.connected == 0u &&
            state.error_operation == H2_PAL_MQTT_OPERATION_CONNECT && state.error_result == rc && !state.error_connected);
        CHECK(h2_pal_mqtt_process(api, client, 20u) == H2_PAL_ERR_INVALID_STATE);
        result->passed = 1;
        goto done;
    }
    CHECK(rc == H2_PAL_OK && state.connected == 1u && state.session_present == 0);
    connected = 1;
    switch (which) {
        case H2_PAL_MQTT_E2E_CONNECT_EVENTS:
        case H2_PAL_MQTT_E2E_AUTHENTICATED:
            break;
        case H2_PAL_MQTT_E2E_REPEATED_CONNECT:
            CHECK(h2_pal_mqtt_connect(api, client) == H2_PAL_ERR_INVALID_STATE && state.connected == 1u);
            break;
        case H2_PAL_MQTT_E2E_SUBSCRIBE_QOS0:
        case H2_PAL_MQTT_E2E_SUBSCRIBE_QOS1:
            rc = subscribe(config, client, &state, topic,
                which == H2_PAL_MQTT_E2E_SUBSCRIBE_QOS1 ? H2_PAL_MQTT_QOS1 : H2_PAL_MQTT_QOS0);
            CHECK(rc == H2_PAL_OK);
            break;
        case H2_PAL_MQTT_E2E_SUBSCRIBE_MULTI: {
            char first_span[256], second_span[256];
            memcpy(first_span, topic, strlen(topic)); memcpy(second_span, other_topic, strlen(other_topic));
            h2_pal_mqtt_subscribe_item_t items[] = {
                {.filter = {first_span, strlen(topic)}, .qos = H2_PAL_MQTT_QOS0},
                {.filter = {second_span, strlen(other_topic)}, .qos = H2_PAL_MQTT_QOS1},
            };
            request.items = items; request.item_count = 2u;
            rc = h2_pal_mqtt_subscribe(api, client, &request, &packet_id);
            CHECK(rc == H2_PAL_OK && packet_id != 0u);
            memset(first_span, 'x', sizeof(first_span)); memset(second_span, 'x', sizeof(second_span));
            rc = wait_for(config, client, &state.subacks, 1u, config->timeout_ms);
            CHECK(rc == H2_PAL_OK && state.sub_id == packet_id && state.sub_count == 2u &&
                state.sub_results[0] == H2_PAL_MQTT_SUBACK_QOS0 && state.sub_results[1] == H2_PAL_MQTT_SUBACK_QOS1);
            rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
            rc = wait_for(config, client, &state.received, 1u, config->timeout_ms); CHECK(rc == H2_PAL_OK);
            state.topic = other_topic; message.topic = (h2_pal_mqtt_str_t){other_topic, strlen(other_topic)};
            rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
            rc = wait_for(config, client, &state.received, 2u, config->timeout_ms); CHECK(rc == H2_PAL_OK);
            break;
        }
        case H2_PAL_MQTT_E2E_INVALID_QOS:
            message.qos = (h2_pal_mqtt_qos_t)2;
            CHECK(h2_pal_mqtt_publish(api, client, &message, &packet_id) == H2_PAL_ERR_UNSUPPORTED && packet_id == 0u);
            item.qos = (h2_pal_mqtt_qos_t)2;
            CHECK(h2_pal_mqtt_subscribe(api, client, &request, &packet_id) == H2_PAL_ERR_UNSUPPORTED && packet_id == 0u);
            break;
        case H2_PAL_MQTT_E2E_INVALID_PUBLISH:
            message.topic.len = 0u;
            CHECK(h2_pal_mqtt_publish(api, client, &message, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            message.topic.len = strlen(topic); message.payload.data = NULL;
            CHECK(h2_pal_mqtt_publish(api, client, &message, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            break;
        case H2_PAL_MQTT_E2E_INVALID_SUBSCRIBE:
            request.item_count = 0u;
            CHECK(h2_pal_mqtt_subscribe(api, client, &request, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            request.item_count = 1u; item.filter.len = 0u;
            CHECK(h2_pal_mqtt_subscribe(api, client, &request, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            break;
        case H2_PAL_MQTT_E2E_INVALID_UNSUBSCRIBE:
            unsubscribe_request.filter_count = 0u;
            CHECK(h2_pal_mqtt_unsubscribe(api, client, &unsubscribe_request, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            unsubscribe_request.filter_count = 1u; filter.len = 0u;
            CHECK(h2_pal_mqtt_unsubscribe(api, client, &unsubscribe_request, &packet_id) == H2_PAL_ERR_INVALID_ARG);
            break;
        case H2_PAL_MQTT_E2E_IDLE_PROCESS: {
            uint64_t start = now(config);
            rc = h2_pal_mqtt_process(api, client, 50u);
            CHECK((rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT) && now(config) - start < 500u && state.received == 0u);
            break;
        }
        case H2_PAL_MQTT_E2E_LOCAL_DISCONNECT:
        case H2_PAL_MQTT_E2E_RECONNECT:
            rc = h2_pal_mqtt_disconnect(api, client, 100u); connected = 0;
            CHECK(rc == H2_PAL_OK && state.disconnected == 1u && state.reason == H2_PAL_MQTT_DISCONNECT_REASON_LOCAL && state.disconnect_result == H2_PAL_OK);
            CHECK(h2_pal_mqtt_disconnect(api, client, 100u) == H2_PAL_ERR_INVALID_STATE);
            if (which == H2_PAL_MQTT_E2E_RECONNECT) {
                rc = h2_pal_mqtt_connect(api, client);
                CHECK(rc == H2_PAL_OK && state.connected == 2u); connected = 1;
                rc = subscribe(config, client, &state, topic, H2_PAL_MQTT_QOS0); CHECK(rc == H2_PAL_OK);
                rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
                rc = wait_for(config, client, &state.received, 1u, config->timeout_ms); CHECK(rc == H2_PAL_OK);
            }
            break;
        case H2_PAL_MQTT_E2E_REMOTE_DISCONNECT:
        case H2_PAL_MQTT_E2E_KEEPALIVE_TIMEOUT:
            rc = wait_for(config, client, &state.disconnected, 1u, 4000u); connected = 0;
            CHECK(rc == H2_PAL_OK && state.disconnected == 1u && state.reason ==
                (which == H2_PAL_MQTT_E2E_REMOTE_DISCONNECT ? H2_PAL_MQTT_DISCONNECT_REASON_TRANSPORT_ERROR : H2_PAL_MQTT_DISCONNECT_REASON_KEEPALIVE_TIMEOUT));
            CHECK(state.disconnect_result == (which == H2_PAL_MQTT_E2E_REMOTE_DISCONNECT ? H2_PAL_ERR_IO : H2_PAL_ERR_TIMEOUT));
            CHECK(h2_pal_mqtt_publish(api, client, &message, NULL) == H2_PAL_ERR_INVALID_STATE);
            break;
        case H2_PAL_MQTT_E2E_QOS_CAPACITY: {
            message.qos = H2_PAL_MQTT_QOS1;
            CHECK(config->qos_publish_capacity > 0u && config->qos_publish_capacity <= 64u);
            for (unsigned i = 0u; i < config->qos_publish_capacity; ++i) {
                rc = h2_pal_mqtt_publish(api, client, &message, &packet_id);
                CHECK(rc == H2_PAL_OK && packet_id != 0u);
            }
            rc = h2_pal_mqtt_publish(api, client, &message, &packet_id);
            CHECK(rc == H2_PAL_ERR_NO_MEMORY && packet_id == 0u && state.errors == 1u &&
                state.error_operation == H2_PAL_MQTT_OPERATION_PUBLISH && state.error_result == rc && state.error_connected);
            rc = wait_for(config, client, &state.pubacks, config->qos_publish_capacity, config->timeout_ms); CHECK(rc == H2_PAL_OK);
            rc = h2_pal_mqtt_publish(api, client, &message, &packet_id); CHECK(rc == H2_PAL_OK);
            rc = wait_for(config, client, &state.pubacks, config->qos_publish_capacity + 1u, config->timeout_ms); CHECK(rc == H2_PAL_OK);
            break;
        }
        case H2_PAL_MQTT_E2E_RETAINED_DELIVERY:
            message.retain = 1;
            rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
            rc = subscribe(config, client, &state, topic, H2_PAL_MQTT_QOS0); CHECK(rc == H2_PAL_OK);
            rc = wait_for(config, client, &state.received, 1u, config->timeout_ms);
            CHECK(rc == H2_PAL_OK && state.retain == 1);
            message.payload = (h2_pal_mqtt_bytes_t){NULL, 0u};
            rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
            break;
        case H2_PAL_MQTT_E2E_REPEATED_LIFECYCLE:
            for (unsigned i = 0u; i < 20u; ++i) {
                rc = subscribe(config, client, &state, topic, H2_PAL_MQTT_QOS0); CHECK(rc == H2_PAL_OK);
                rc = h2_pal_mqtt_publish(api, client, &message, NULL); CHECK(rc == H2_PAL_OK);
                rc = wait_for(config, client, &state.received, i + 1u, config->timeout_ms); CHECK(rc == H2_PAL_OK);
                rc = h2_pal_mqtt_disconnect(api, client, 100u); connected = 0; CHECK(rc == H2_PAL_OK);
                h2_pal_mqtt_close(api, client); client = NULL;
                rc = h2_pal_mqtt_open(api, &client_config, &client); CHECK(rc == H2_PAL_OK);
                rc = h2_pal_mqtt_connect(api, client); CHECK(rc == H2_PAL_OK); connected = 1;
            }
            CHECK(state.connected == 21u && state.disconnected == 20u && state.received == 20u);
            break;
        default: {
            h2_pal_mqtt_qos_t qos = which == H2_PAL_MQTT_E2E_PUBLISH_QOS1 ? H2_PAL_MQTT_QOS1 : H2_PAL_MQTT_QOS0;
            if (which == H2_PAL_MQTT_E2E_PUBLISH_EMPTY) state.payload_len = 0u;
            if (which == H2_PAL_MQTT_E2E_PUBLISH_BINARY) state.payload_len = 257u;
            if (which == H2_PAL_MQTT_E2E_PUBLISH_LARGE) state.payload_len = sizeof(scratch->payload);
            message.payload.len = state.payload_len; message.qos = qos;
            rc = subscribe(config, client, &state, topic, qos); CHECK(rc == H2_PAL_OK);
            if (which == H2_PAL_MQTT_E2E_UNSUBSCRIBE_ACK) {
                char unsubscribe_span[256];
                memcpy(unsubscribe_span, topic, strlen(topic)); filter.data = unsubscribe_span;
                rc = h2_pal_mqtt_unsubscribe(api, client, &unsubscribe_request, &packet_id); CHECK(rc == H2_PAL_OK);
                memset(unsubscribe_span, 'x', sizeof(unsubscribe_span));
                rc = wait_for(config, client, &state.unsubacks, 1u, config->timeout_ms);
                CHECK(rc == H2_PAL_OK && packet_id != 0u && state.unsub_id == packet_id);
            }
            if (which == H2_PAL_MQTT_E2E_CALLBACK_CLOSE) state.close_on_publish = 1;
            char topic_span[256];
            if (which == H2_PAL_MQTT_E2E_INPUT_LIFETIME) {
                memcpy(topic_span, topic, strlen(topic)); message.topic.data = topic_span;
            }
            rc = h2_pal_mqtt_publish(api, client, &message, &packet_id);
            CHECK(rc == H2_PAL_OK && (qos == H2_PAL_MQTT_QOS0 ? packet_id == 0u : packet_id != 0u));
            if (which == H2_PAL_MQTT_E2E_INPUT_LIFETIME) {
                memset(scratch->payload, 0xee, message.payload.len); memset(topic_span, 'x', sizeof(topic_span));
            }
            if (which == H2_PAL_MQTT_E2E_UNSUBSCRIBE_ACK) {
                rc = wait_for(config, client, &state.received, 1u, 150u);
                CHECK(rc == H2_PAL_ERR_TIMEOUT && state.received == 0u);
            } else {
                rc = wait_for(config, client, &state.received, 1u, config->timeout_ms);
                CHECK(rc == H2_PAL_OK && state.received == 1u && state.qos == qos);
            }
            if (qos == H2_PAL_MQTT_QOS1) {
                rc = wait_for(config, client, &state.pubacks, 1u, config->timeout_ms);
                CHECK(rc == H2_PAL_OK && state.pub_id == packet_id && state.pubacks == 1u);
            }
            if (state.closed_in_callback) { connected = 0; CHECK(client == NULL); }
            break;
        }
    }
    CHECK(!state.invalid);
    result->passed = 1;
done:
    if (client != NULL) {
        if (connected) {
            unsigned before = state.disconnected;
            int cleanup = h2_pal_mqtt_disconnect(api, client, 100u);
            if (cleanup != H2_PAL_OK || state.disconnected != before + 1u || state.reason != H2_PAL_MQTT_DISCONNECT_REASON_LOCAL) {
                result->passed = 0; result->detail = cleanup == H2_PAL_OK ? H2_PAL_ERR_IO : cleanup;
            }
        }
        h2_pal_mqtt_close(api, client);
    }
    if (state.invalid || (state.errors != 0u && which != H2_PAL_MQTT_E2E_QOS_CAPACITY &&
        which != H2_PAL_MQTT_E2E_AUTH_REFUSED && which != H2_PAL_MQTT_E2E_CONNECT_TIMEOUT &&
        which != H2_PAL_MQTT_E2E_TLS_UNTRUSTED && which != H2_PAL_MQTT_E2E_TLS_WRONG_NAME)) {
        result->passed = 0; result->detail = H2_PAL_ERR_IO;
    }
    result->connected = state.connected;
    result->received = state.received;
    result->disconnected = state.disconnected;
}

int h2_pal_mqtt_e2e_run(const h2_pal_mqtt_e2e_config_t *config,
                      h2_pal_mqtt_e2e_result_t *out_result) {
    if (out_result == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out_result, 0, sizeof(*out_result));
    if (config == NULL || config->runtime == NULL || config->host == NULL || config->host[0] == '\0' ||
        config->session == NULL || config->session[0] == '\0' || config->topic_prefix == NULL ||
        config->topic_prefix[0] == '\0' || config->timeout_ms == 0u || config->tcp_port == 0u) return H2_PAL_ERR_INVALID_ARG;
    const h2_pal_mqtt_api_t *api = config->runtime->mqtt;
    const h2_pal_mqtt_vtable_t *v = api == NULL ? NULL : api->vtable;
    int available = v != NULL && v->open != NULL && v->connect != NULL && v->disconnect != NULL &&
        v->publish != NULL && v->subscribe != NULL && v->unsubscribe != NULL && v->process != NULL && v->close != NULL &&
        config->runtime->time != NULL && config->runtime->time->vtable != NULL &&
        config->runtime->time->vtable->get_monotonic_ms != NULL && config->runtime->mem != NULL &&
        config->runtime->mem->vtable != NULL && config->runtime->mem->vtable->alloc != NULL &&
        config->runtime->mem->vtable->free != NULL;
    int blocked_result = H2_PAL_ERR_UNSUPPORTED;
    if (available) {
        uint64_t monotonic = 0u;
        int clock_result = h2_pal_time_get_monotonic_ms(config->runtime->time, &monotonic);
        if (clock_result != H2_PAL_OK) { available = 0; blocked_result = clock_result; }
    }
    scratch_t *scratch = available ? h2_pal_mem_alloc(config->runtime->mem, sizeof(*scratch)) : NULL;
    for (unsigned index = 0u; index < H2_PAL_MQTT_E2E_CASE_COUNT; ++index) {
        h2_pal_mqtt_e2e_case_result_t *result = &out_result->cases[index];
        result->id = case_ids[index];
        if (config->smoke_only && index != H2_PAL_MQTT_E2E_PUBLISH_QOS0) continue;
        ++out_result->selected;
        if (!available || scratch == NULL) { result->blocked = 1; result->detail = !available ? blocked_result : H2_PAL_ERR_NO_MEMORY; }
        else {
            uint64_t start = now(config);
            run_case(config, (h2_pal_mqtt_e2e_case_t)index, scratch, result);
            result->elapsed_ms = now(config) - start;
        }
        if (result->passed) ++out_result->passed;
        else if (result->blocked) ++out_result->blocked;
        else ++out_result->failed;
        if (config->report != NULL) config->report(config->report_user, result);
    }
    h2_pal_mem_free(config->runtime->mem, scratch);
    return out_result->failed == 0u && out_result->blocked == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}
