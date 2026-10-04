#ifndef H2_MQTT_MOBILE_RUNNER_H
#define H2_MQTT_MOBILE_RUNNER_H
#include "h2_pal_mqtt_e2e.h"
typedef struct h2_mqtt_mobile_result {
    h2_pal_mqtt_e2e_result_t suite;
    size_t before[9], after[9];
    size_t case_allocations[H2_PAL_MQTT_E2E_CASE_COUNT];
    size_t retained_allocations;
    unsigned invalid_frees, owner_failure_verified;
    int owner_destroy;
} h2_mqtt_mobile_result_t;
/* Borrow all injected spans through return; create/destroy the SDK MQTT owner. */
int h2_mqtt_mobile_run(h2_runtime_config_t config, const char *host,
    uint16_t tcp_port, uint16_t tls_port, const char *session,
    const uint8_t *ca, size_t ca_len, const uint8_t *wrong_ca, size_t wrong_ca_len,
    h2_mqtt_mobile_result_t *out);
int h2_mqtt_mobile_report(const char *path, const char *platform, const char *version,
    const h2_mqtt_mobile_result_t *result, int rc, int teardown);
#endif
