#ifndef H2_COREMQTT_H
#define H2_COREMQTT_H

#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/application/h2_pal_mqtt.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_coremqtt h2_coremqtt_t;

typedef struct h2_coremqtt_config {
    const h2_pal_mem_api_t *allocator;
    const h2_pal_net_api_t *net;
    const h2_pal_time_api_t *time;
    const h2_pal_log_api_t *log;
    size_t outgoing_publish_records;
    size_t incoming_publish_records;
} h2_coremqtt_config_t;

/*
 * Creates a fixed coreMQTT-backed implementation of h2_pal_mqtt_api_t.
 *
 * The returned h2_coremqtt_t owns out_api->user and the vtable remains valid
 * until h2_coremqtt_destroy(). Per-client handles returned by open() are owned
 * by the API and must be released with close() before destroying the provider.
 *
 * h2_pal_mqtt_client_config_t string/byte views are borrowed for the lifetime
 * of the opened client. network_buffer is caller-owned and must remain valid
 * until close().
 */
h2_pal_result_t h2_coremqtt_create(
    const h2_coremqtt_config_t *config,
    h2_coremqtt_t **out_mqtt,
    h2_pal_mqtt_api_t *out_api);

void h2_coremqtt_destroy(h2_coremqtt_t *mqtt);

#ifdef __cplusplus
}
#endif

#endif
