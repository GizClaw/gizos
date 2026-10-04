#ifndef H2_PAL_MQTT_E2E_H
#define H2_PAL_MQTT_E2E_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_mqtt_e2e_case {
#define H2_PAL_MQTT_CASE(symbol, id) H2_PAL_MQTT_E2E_##symbol,
#include "h2_pal_mqtt_cases.inc"
#undef H2_PAL_MQTT_CASE
    H2_PAL_MQTT_E2E_CASE_COUNT
} h2_pal_mqtt_e2e_case_t;

typedef struct h2_pal_mqtt_e2e_case_result {
    const char *id;
    int passed;
    int blocked;
    int detail;
    unsigned line;
    uint64_t elapsed_ms;
    unsigned connected;
    unsigned received;
    unsigned disconnected;
} h2_pal_mqtt_e2e_case_result_t;

typedef struct h2_pal_mqtt_e2e_result {
    unsigned selected;
    unsigned passed;
    unsigned failed;
    unsigned blocked;
    h2_pal_mqtt_e2e_case_result_t cases[H2_PAL_MQTT_E2E_CASE_COUNT];
} h2_pal_mqtt_e2e_result_t;

typedef struct h2_pal_mqtt_e2e_config {
    /** Borrowed Runtime with a real MQTT provider and monotonic clock. */
    const h2_runtime_t *runtime;
    const char *host;
    uint16_t tcp_port;
    uint16_t tls_port;
    const h2_pal_net_tls_config_t *trusted_tls;
    const h2_pal_net_tls_config_t *untrusted_tls;
    const h2_pal_net_tls_config_t *wrong_name_tls;
    /** Unique run identity; topic_prefix must be an owned, broker-safe prefix. */
    const char *session;
    const char *topic_prefix;
    uint32_t timeout_ms;
    /** Outgoing QoS1 record capacity selected by the platform provider assembly. */
    unsigned qos_publish_capacity;
    /** Only the legacy-equivalent QoS0 round trip, for a real public broker. */
    int smoke_only;
    h2_pal_mqtt_transport_t smoke_transport;
    void (*report)(void *user, const h2_pal_mqtt_e2e_case_result_t *result);
    void *report_user;
} h2_pal_mqtt_e2e_config_t;

/** Run the independent registry without assembling a platform or broker.
 * Full qualification requires the documented controlled MQTT fixture behavior.
 * Every case owns and closes its client; no callback is retained after return.
 * Missing provider callbacks block all selected cases. */
int h2_pal_mqtt_e2e_run(const h2_pal_mqtt_e2e_config_t *config,
                      h2_pal_mqtt_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif
#endif
