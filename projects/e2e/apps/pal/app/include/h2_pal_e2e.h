#ifndef H2_PAL_E2E_H
#define H2_PAL_E2E_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_E2E_MQTT_DEFAULT_TIMEOUT_MS 5000u
#define H2_PAL_E2E_MAX_CASES 32u

typedef enum h2_pal_e2e_suite {
  H2_PAL_E2E_SUITE_CORE = 1u << 0,
  H2_PAL_E2E_SUITE_MQTT = 1u << 1,
  H2_PAL_E2E_SUITE_PREF = 1u << 2,
  H2_PAL_E2E_SUITE_HOST = 1u << 3,
} h2_pal_e2e_suite_t;

typedef enum h2_pal_e2e_case_id {
  H2_PAL_E2E_CASE_TIME = 1,
  H2_PAL_E2E_CASE_TIMER,
  H2_PAL_E2E_CASE_TASK,
  H2_PAL_E2E_CASE_QUEUE,
  H2_PAL_E2E_CASE_MUTEX,
  H2_PAL_E2E_CASE_SEMAPHORE,
  H2_PAL_E2E_CASE_UNSUPPORTED = 8,
  H2_PAL_E2E_CASE_MQTT,
  H2_PAL_E2E_CASE_CONDITION,
  H2_PAL_E2E_CASE_CONCURRENCY,
  H2_PAL_E2E_CASE_HOST_MEMORY,
  H2_PAL_E2E_CASE_HOST_FILESYSTEM,
  H2_PAL_E2E_CASE_HOST_RESOLVE_SYNC,
  H2_PAL_E2E_CASE_HOST_RESOLVE_ASYNC,
  H2_PAL_E2E_CASE_HOST_UDP_IPV4,
  H2_PAL_E2E_CASE_HOST_UDP_IPV6,
  H2_PAL_E2E_CASE_HOST_TCP,
  H2_PAL_E2E_CASE_HOST_TLS,
  H2_PAL_E2E_CASE_HOST_TLS_WRONG_CA,
  H2_PAL_E2E_CASE_HOST_HTTPS,
  H2_PAL_E2E_CASE_HOST_MQTT,
  H2_PAL_E2E_CASE_HOST_NETIF,
  H2_PAL_E2E_CASE_HOST_SYSTEM_EVENT,
} h2_pal_e2e_case_id_t;

typedef struct h2_pal_e2e_case_result {
  h2_pal_e2e_case_id_t case_id;
  h2_pal_result_t result;
} h2_pal_e2e_case_result_t;

typedef struct h2_pal_e2e_cleanup h2_pal_e2e_cleanup_t;

typedef enum h2_pal_e2e_action {
  H2_PAL_E2E_ACTION_NONE = 0,
  H2_PAL_E2E_ACTION_REBOOT,
} h2_pal_e2e_action_t;

typedef enum h2_pal_e2e_pref_phase {
  H2_PAL_E2E_PREF_PHASE_SEED = 0,
  H2_PAL_E2E_PREF_PHASE_VERIFY,
  H2_PAL_E2E_PREF_PHASE_CLEAN,
  H2_PAL_E2E_PREF_PHASE_COMPLETE,
} h2_pal_e2e_pref_phase_t;

typedef enum h2_pal_e2e_stage {
  H2_PAL_E2E_STAGE_PREFLIGHT = 0,
  H2_PAL_E2E_STAGE_OPEN,
  H2_PAL_E2E_STAGE_CONNECT,
  H2_PAL_E2E_STAGE_SUBSCRIBE,
  H2_PAL_E2E_STAGE_PUBLISH,
  H2_PAL_E2E_STAGE_DISCONNECT,
  H2_PAL_E2E_STAGE_COMPLETE,
} h2_pal_e2e_stage_t;

/**
 * Borrowed MQTT case inputs supplied by the target launcher.
 *
 * Every string, byte view, and network buffer must remain valid until
 * h2_pal_e2e_run() returns. The launcher owns endpoint selection, unique test
 * values, and buffer storage. A zero timeout selects the fixed default.
 */
typedef struct h2_pal_e2e_mqtt_config {
  /** Borrowed NUL-terminated hostname used for MQTT and TLS SNI. */
  const char *host;
  uint16_t port;
  h2_pal_mqtt_transport_t transport;
  h2_pal_mqtt_str_t client_id;
  h2_pal_mqtt_str_t topic;
  h2_pal_mqtt_bytes_t payload;
  uint32_t timeout_ms;
  uint8_t *network_buffer;
  size_t network_buffer_len;
} h2_pal_e2e_mqtt_config_t;

typedef struct h2_pal_e2e_config {
  uint32_t suite_mask;
  struct {
    uint16_t tcp_echo_port;
    uint16_t tls_echo_port;
    uint16_t tls_wrong_ca_port;
    uint16_t https_port;
    uint32_t timeout_ms;
    const uint8_t *root_ca_pem;
    size_t root_ca_pem_len;
    const uint8_t *wrong_ca_pem;
    size_t wrong_ca_pem_len;
  } host;
  h2_pal_e2e_mqtt_config_t mqtt;
} h2_pal_e2e_config_t;

typedef struct h2_pal_e2e_result {
  h2_pal_e2e_stage_t stage;
  h2_pal_result_t result;
  h2_pal_e2e_case_result_t cases[H2_PAL_E2E_MAX_CASES];
  size_t case_count;
  size_t selected;
  size_t passed;
  size_t failed;
  int connected_events;
  int subscribe_ack_events;
  int publish_echo_events;
  int disconnected_events;
  h2_pal_e2e_pref_phase_t pref_phase;
  h2_pal_e2e_action_t action;
  int complete;
  h2_pal_result_t cleanup_result;
  h2_pal_e2e_cleanup_t *retained_cleanup;
} h2_pal_e2e_result_t;

/**
 * Runs the blocking portable PAL E2E registry and closes all App-owned handles.
 *
 * Core and MQTT may be selected together. Preference must be selected alone
 * because it returns cross-boot actions. MQTT requires MQTT and monotonic
 * Time; Preference requires Memory and Preference. The output is initialized
 * on every call and records the terminal phase/stage even when validation or
 * an operation fails.
 *
 * @param runtime Borrowed initialized Runtime.
 * @param config Borrowed launcher-provided case configuration.
 * @param out_result Caller-provided result storage.
 * @return H2_PAL_OK only when every selected case passes.
 */
h2_pal_result_t h2_pal_e2e_run(h2_runtime_t *runtime,
                               const h2_pal_e2e_config_t *config,
                               h2_pal_e2e_result_t *out_result);

/**
 * Retries cleanup retained after a Task join failure.
 *
 * The caller must keep the Runtime alive and call this function again after
 * making platform progress while it returns H2_PAL_ERR_BUSY. A successful
 * call releases every retained Task handle and synchronization object.
 */
h2_pal_result_t h2_pal_e2e_cleanup(h2_runtime_t *runtime,
                                   h2_pal_e2e_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
