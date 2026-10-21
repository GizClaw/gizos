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
int h2_mqtt_device_prepare(h2_runtime_t* runtime);
/** Borrow optional fixture credentials until return; NULL retains old setup.
 * Uses non-saving PAL connect and the original association/IP wait budgets. */
int h2_mqtt_device_prepare_with_wifi(h2_runtime_t* runtime,
                                     const h2_pal_wifi_sta_config_t* wifi);
typedef int (*h2_mqtt_device_digest_fn)(void* user, const uint8_t* bytes,
                                        size_t length, uint8_t digest[32]);
int h2_mqtt_device_run(h2_runtime_t* runtime, unsigned outgoing_capacity,
                       h2_mqtt_device_digest_fn digest, void* digest_user,
                       h2_mqtt_device_result_t* out);
/* Return only after every first-ledger record was accepted by the log sink. */
int h2_mqtt_device_replay(h2_runtime_t* runtime,
                          const h2_mqtt_device_result_t* result);
#endif
