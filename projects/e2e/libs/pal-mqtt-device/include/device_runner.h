#ifndef H2_PAL_MQTT_DEVICE_RUNNER_H
#define H2_PAL_MQTT_DEVICE_RUNNER_H
#include "h2_pal_mqtt_e2e.h"
typedef struct h2_mqtt_device_result {
    h2_pal_mqtt_e2e_result_t suite;
    char execution[96], ca_sha256[65];
    uint64_t epoch_ms;
    size_t before[10], after[10];
    int rc, cleanup;
} h2_mqtt_device_result_t;
extern const char h2_pal_mqtt_device_runner_task_name[];
int h2_mqtt_device_prepare(h2_runtime_t *runtime);
int h2_mqtt_device_run(h2_runtime_t *runtime, unsigned outgoing_capacity, h2_mqtt_device_result_t *out);
void h2_mqtt_device_replay(h2_runtime_t *runtime, const h2_mqtt_device_result_t *result);
#endif
