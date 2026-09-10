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
#define H2_GIZCLAW_TELEMETRY_IMEI_LEN 15u
#define H2_GIZCLAW_TELEMETRY_IMSI_MIN_LEN 6u
#define H2_GIZCLAW_TELEMETRY_IMSI_MAX_LEN 15u

typedef enum h2_gizclaw_telemetry_kind {
  H2_GIZCLAW_TELEMETRY_BATTERY = 1,
  H2_GIZCLAW_TELEMETRY_GNSS,
  H2_GIZCLAW_TELEMETRY_NETWORK,
  H2_GIZCLAW_TELEMETRY_SYSTEM,
  H2_GIZCLAW_TELEMETRY_AUDIOPLAYER,
  H2_GIZCLAW_TELEMETRY_OTA,
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
  /*
   * Cellular subscriber identity, borrowed for the duration of
   * h2_gizclaw_req_create_telemetry_send() and copied into the request.
   * imei is exactly H2_GIZCLAW_TELEMETRY_IMEI_LEN ASCII decimal digits; imsi is
   * H2_GIZCLAW_TELEMETRY_IMSI_MIN_LEN to H2_GIZCLAW_TELEMETRY_IMSI_MAX_LEN of
   * them. An empty span is the same as leaving has_* false. Both are rejected
   * with H2_PAL_ERR_INVALID_ARG when rat compares equal to "wifi", ignoring
   * case, matching the server rule. Neither value is logged or traced.
   */
  bool has_imei;
  h2_gizclaw_str_t imei;
  bool has_imsi;
  h2_gizclaw_str_t imsi;
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

typedef enum h2_gizclaw_ota_state {
  H2_GIZCLAW_OTA_STATE_UNSPECIFIED = 0,
  H2_GIZCLAW_OTA_STATE_STARTED = 1,
  H2_GIZCLAW_OTA_STATE_DOWNLOADING = 2,
  H2_GIZCLAW_OTA_STATE_SUCCEEDED = 3,
  H2_GIZCLAW_OTA_STATE_FAILED = 4,
} h2_gizclaw_ota_state_t;

typedef struct {
  h2_gizclaw_str_t state;
  bool has_current_index;
  uint32_t current_index;
  uint64_t position_ms;
  bool has_duration_ms;
  uint64_t duration_ms;
  h2_gizclaw_str_t repeat;
  uint32_t playlist_length;
  uint32_t playlist_revision;
  bool has_error_code;
  h2_gizclaw_str_t error_code;
  bool has_error_message;
  h2_gizclaw_str_t error_message;
} h2_gizclaw_telemetry_audioplayer_t;

typedef struct {
  h2_gizclaw_ota_state_t state;
  h2_gizclaw_str_t update_id;
  bool has_target_version;
  h2_gizclaw_str_t target_version;
  bool has_download_percent;
  double download_percent;
  bool has_error_code;
  h2_gizclaw_str_t error_code;
  bool has_error_message;
  h2_gizclaw_str_t error_message;
} h2_gizclaw_telemetry_ota_t;

typedef struct h2_gizclaw_telemetry_observation {
  int32_t observed_at_delta_ms;
  h2_gizclaw_telemetry_kind_t kind;
  union {
    h2_gizclaw_telemetry_battery_t battery;
    h2_gizclaw_telemetry_gnss_t gnss;
    h2_gizclaw_telemetry_network_t network;
    h2_gizclaw_telemetry_system_t system;
    h2_gizclaw_telemetry_audioplayer_t audioplayer;
    h2_gizclaw_telemetry_ota_t ota;
  } value;
} h2_gizclaw_telemetry_observation_t;

/**
 * Borrowed, bounded telemetry frame submitted on the GizClaw owner task.
 *
 * OTA frames contain exactly one OTA observation (SDK dedicated frame API).
 * Every string and observation is borrowed only for the duration of
 * h2_gizclaw_req_create_telemetry_send(). Missing facts stay unset.
 */
typedef struct h2_gizclaw_telemetry_frame {
  uint32_t sequence;
  int64_t observed_at_unix_ms;
  const h2_gizclaw_telemetry_observation_t *observations;
  size_t observation_count;
} h2_gizclaw_telemetry_frame_t;

/** Copy the frame and its strings into a CREATED request. */
h2_pal_result_t h2_gizclaw_req_create_telemetry_send(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_telemetry_frame_t *frame, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
/** Telemetry is a one-way packet: success confirms transport acceptance,
 * not a server acknowledgement or persisted observation. Submission runs once
 * on the Service owner task. WOULD_BLOCK is a terminal result returned by wait,
 * this parser and the synchronous RPC; the library never retries the packet. */
h2_pal_result_t
h2_gizclaw_resp_parse_telemetry_send(const h2_gizclaw_req_t *request);
h2_pal_result_t
h2_gizclaw_rpc_telemetry_send(h2_gizclaw_service_t *service,
                              const h2_gizclaw_telemetry_frame_t *frame,
                              uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
