#ifndef H2_BLEIKCP_INTERNAL_H
#define H2_BLEIKCP_INTERNAL_H

#include "h2_bleikcp.h"
#include "ikcp.h"

#include <stdbool.h>

#define H2_BLEIKCP_KCP_OVERHEAD 24u
#define H2_BLEIKCP_UUID_MAX_LEN 16u
#define H2_BLEIKCP_SUBSCRIPTION_COUNT 2u

typedef enum h2_bleikcp_role {
    H2_BLEIKCP_ROLE_CLIENT = 0,
    H2_BLEIKCP_ROLE_SERVER = 1,
} h2_bleikcp_role_t;

typedef struct h2_bleikcp_ring {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t len;
} h2_bleikcp_ring_t;

typedef struct h2_bleikcp_frame_queue {
    uint8_t *data;
    uint16_t *lengths;
    size_t capacity;
    size_t frame_size;
    size_t head;
    size_t count;
} h2_bleikcp_frame_queue_t;

typedef struct h2_bleikcp_resolved_config {
    h2_bleikcp_config_t value;
    uint8_t service_uuid[H2_BLEIKCP_UUID_MAX_LEN];
    uint8_t tx_uuid[H2_BLEIKCP_UUID_MAX_LEN];
    uint8_t rx_uuid[H2_BLEIKCP_UUID_MAX_LEN];
} h2_bleikcp_resolved_config_t;

struct h2_bleikcp {
    h2_bleikcp_api_t api;
    h2_bleikcp_resolved_config_t config;
    h2_bleikcp_role_t role;
    uint16_t conn_handle;
    uint16_t att_mtu;
    uint16_t kcp_mtu;
    uint16_t tx_value_handle;
    uint16_t tx_cccd_handle;
    uint16_t rx_value_handle;
    h2_pal_mutex_t *mutex;
    h2_pal_cond_t *cond;
    h2_pal_task_t *worker;
    ikcpcb *kcp;
    h2_bleikcp_ring_t tx;
    h2_bleikcp_ring_t rx;
    h2_bleikcp_frame_queue_t input;
    uint8_t *scratch;
    bool closing;
    bool worker_started;
    bool borrowed;
    bool connected_event_pending;
    bool ready_event_pending;
    bool disconnect_event_pending;
    int fatal_status;
    h2_bleikcp_stats_t stats;
    h2_pal_system_event_subscription_t *subscriptions[H2_BLEIKCP_SUBSCRIPTION_COUNT];
};

int h2_bleikcp_resolve_config(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    h2_bleikcp_resolved_config_t *out);
int h2_bleikcp_stream_create(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_resolved_config_t *config,
    h2_bleikcp_role_t role,
    uint16_t conn_handle,
    uint16_t att_mtu,
    bool borrowed,
    h2_bleikcp_t **out_stream);
int h2_bleikcp_stream_start(h2_bleikcp_t *stream);
void h2_bleikcp_stream_mark_closed(h2_bleikcp_t *stream, int status, bool disconnected);
int h2_bleikcp_stream_join(h2_bleikcp_t *stream);
int h2_bleikcp_stream_destroy(h2_bleikcp_t *stream);
int h2_bleikcp_stream_input(h2_bleikcp_t *stream, const uint8_t *data, size_t len);
bool h2_bleikcp_uuid_equal(const h2_pal_ble_uuid_t *a, const h2_pal_ble_uuid_t *b);

#endif
