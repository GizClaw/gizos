#ifndef H2_GIZCLAW_TELEMETRY_H
#define H2_GIZCLAW_TELEMETRY_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS 4u
#define H2_GIZCLAW_TELEMETRY_TOKEN_MAX 64u
#define H2_GIZCLAW_TELEMETRY_VERSION_MAX 96u

typedef enum h2_gizclaw_telemetry_kind {
  H2_GIZCLAW_TELEMETRY_BATTERY = 1,
  H2_GIZCLAW_TELEMETRY_GNSS,
  H2_GIZCLAW_TELEMETRY_NETWORK,
  H2_GIZCLAW_TELEMETRY_SYSTEM,
} h2_gizclaw_telemetry_kind_t;

typedef struct h2_gizclaw_telemetry_battery {
  bool has_percent;
  double percent;
  bool has_charging;
  bool charging;
  bool has_voltage_mv;
  double voltage_mv;
} h2_gizclaw_telemetry_battery_t;

typedef struct h2_gizclaw_telemetry_gnss {
  double latitude;
  double longitude;
  bool has_altitude_m;
  double altitude_m;
  bool has_accuracy_m;
  double accuracy_m;
} h2_gizclaw_telemetry_gnss_t;

typedef struct h2_gizclaw_telemetry_network {
  bool has_rssi_dbm;
  double rssi_dbm;
  bool has_signal_level;
  double signal_level;
  bool has_rat;
  h2_gizclaw_str_t rat;
  bool has_operator_name;
  h2_gizclaw_str_t operator_name;
  bool has_connected;
  bool connected;
} h2_gizclaw_telemetry_network_t;

typedef struct h2_gizclaw_telemetry_system {
  bool has_uptime_seconds;
  double uptime_seconds;
  bool has_free_memory_bytes;
  double free_memory_bytes;
  bool has_temperature_c;
  double temperature_c;
  bool has_firmware_version;
  h2_gizclaw_str_t firmware_version;
  bool has_software_version;
  h2_gizclaw_str_t software_version;
  bool has_hardware_version;
  h2_gizclaw_str_t hardware_version;
} h2_gizclaw_telemetry_system_t;

typedef struct h2_gizclaw_telemetry_observation {
  int32_t observed_at_delta_ms;
  h2_gizclaw_telemetry_kind_t kind;
  union {
    h2_gizclaw_telemetry_battery_t battery;
    h2_gizclaw_telemetry_gnss_t gnss;
    h2_gizclaw_telemetry_network_t network;
    h2_gizclaw_telemetry_system_t system;
  } value;
} h2_gizclaw_telemetry_observation_t;

/**
 * Borrowed, bounded telemetry frame submitted on the GizClaw owner task.
 *
 * Every string and observation is borrowed only for the duration of
 * h2_gizclaw_service_telemetry_send_async(). Missing facts stay unset.
 */
typedef struct h2_gizclaw_telemetry_frame {
  uint32_t sequence;
  int64_t observed_at_unix_ms;
  const h2_gizclaw_telemetry_observation_t *observations;
  size_t observation_count;
} h2_gizclaw_telemetry_frame_t;

typedef struct h2_gizclaw_telemetry_request h2_gizclaw_telemetry_request_t;
typedef void (*h2_gizclaw_telemetry_completion_fn)(
    void *user, h2_gizclaw_telemetry_request_t *request);

h2_pal_result_t h2_gizclaw_service_telemetry_send_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_telemetry_frame_t *frame,
    h2_gizclaw_telemetry_completion_fn completion, void *user,
    h2_gizclaw_telemetry_request_t **out_request);
h2_pal_result_t
h2_gizclaw_telemetry_request_cancel(h2_gizclaw_telemetry_request_t *request);
h2_pal_result_t h2_gizclaw_telemetry_request_wait(
    h2_gizclaw_telemetry_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *
h2_gizclaw_telemetry_request_operation_result(
    const h2_gizclaw_telemetry_request_t *request);
void h2_gizclaw_telemetry_request_release(
    h2_gizclaw_telemetry_request_t *request);

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_client_telemetry_send(
    h2_gizclaw_client_t *client,
    const h2_gizclaw_telemetry_frame_t *frame);
#endif

#ifdef __cplusplus
}
#endif

#endif
