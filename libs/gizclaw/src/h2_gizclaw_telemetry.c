#include "h2_gizclaw_telemetry.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "gzc_common.h"
#include "gzc_telemetry.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_test_telemetry_send_fn test_send;
static void *test_send_user;

void h2_gizclaw_test_set_telemetry_send(h2_gizclaw_test_telemetry_send_fn send,
                                        void *user) {
  test_send = send;
  test_send_user = user;
}
#endif

static bool span_valid(h2_gizclaw_str_t value, size_t max_len) {
  return value.data != NULL && value.len > 0u && value.len <= max_len &&
         memchr(value.data, '\0', value.len) == NULL;
}

static bool battery_valid(const h2_gizclaw_telemetry_battery_t *value) {
  if (!value->has_percent && !value->has_charging && !value->has_voltage_mv) {
    return false;
  }
  return (!value->has_percent ||
          (isfinite(value->percent) && value->percent >= 0.0 &&
           value->percent <= 100.0)) &&
         (!value->has_voltage_mv ||
          (isfinite(value->voltage_mv) && value->voltage_mv >= 0.0));
}

static bool gnss_valid(const h2_gizclaw_telemetry_gnss_t *value) {
  return isfinite(value->latitude) && value->latitude >= -90.0 &&
         value->latitude <= 90.0 && isfinite(value->longitude) &&
         value->longitude >= -180.0 && value->longitude <= 180.0 &&
         (!value->has_altitude_m || isfinite(value->altitude_m)) &&
         (!value->has_accuracy_m ||
          (isfinite(value->accuracy_m) && value->accuracy_m >= 0.0));
}

static bool network_valid(const h2_gizclaw_telemetry_network_t *value) {
  if (!value->has_rssi_dbm && !value->has_signal_level && !value->has_rat &&
      !value->has_operator_name && !value->has_connected) {
    return false;
  }
  return (!value->has_rssi_dbm || isfinite(value->rssi_dbm)) &&
         (!value->has_signal_level ||
          (isfinite(value->signal_level) && value->signal_level >= 0.0 &&
           value->signal_level <= 4.0)) &&
         (!value->has_rat ||
          span_valid(value->rat, H2_GIZCLAW_TELEMETRY_TOKEN_MAX)) &&
         (!value->has_operator_name ||
          span_valid(value->operator_name, H2_GIZCLAW_TELEMETRY_TOKEN_MAX));
}

static bool system_valid(const h2_gizclaw_telemetry_system_t *value) {
  if (!value->has_uptime_seconds && !value->has_free_memory_bytes &&
      !value->has_temperature_c && !value->has_firmware_version &&
      !value->has_software_version && !value->has_hardware_version) {
    return false;
  }
  return (!value->has_uptime_seconds ||
          (isfinite(value->uptime_seconds) && value->uptime_seconds >= 0.0)) &&
         (!value->has_free_memory_bytes ||
          (isfinite(value->free_memory_bytes) &&
           value->free_memory_bytes >= 0.0)) &&
         (!value->has_temperature_c || isfinite(value->temperature_c)) &&
         (!value->has_firmware_version ||
          span_valid(value->firmware_version,
                     H2_GIZCLAW_TELEMETRY_VERSION_MAX)) &&
         (!value->has_software_version ||
          span_valid(value->software_version,
                     H2_GIZCLAW_TELEMETRY_VERSION_MAX)) &&
         (!value->has_hardware_version ||
          span_valid(value->hardware_version,
                     H2_GIZCLAW_TELEMETRY_VERSION_MAX));
}

static bool
observation_valid(const h2_gizclaw_telemetry_observation_t *observation) {
  switch (observation->kind) {
  case H2_GIZCLAW_TELEMETRY_BATTERY:
    return battery_valid(&observation->value.battery);
  case H2_GIZCLAW_TELEMETRY_GNSS:
    return gnss_valid(&observation->value.gnss);
  case H2_GIZCLAW_TELEMETRY_NETWORK:
    return network_valid(&observation->value.network);
  case H2_GIZCLAW_TELEMETRY_SYSTEM:
    return system_valid(&observation->value.system);
  default:
    return false;
  }
}

static gzc_str_t to_gzc_str(h2_gizclaw_str_t value) {
  return gzc_str_from_parts(value.data, value.len);
}

static void map_observation(const h2_gizclaw_telemetry_observation_t *source,
                            gzc_telemetry_observation_t *target) {
  memset(target, 0, sizeof(*target));
  target->observed_at_delta_ms = source->observed_at_delta_ms;
  target->kind = (gzc_telemetry_observation_kind_t)source->kind;
  switch (source->kind) {
  case H2_GIZCLAW_TELEMETRY_BATTERY:
    target->battery.has_percent = source->value.battery.has_percent;
    target->battery.percent = source->value.battery.percent;
    target->battery.has_charging = source->value.battery.has_charging;
    target->battery.charging = source->value.battery.charging;
    target->battery.has_voltage_mv = source->value.battery.has_voltage_mv;
    target->battery.voltage_mv = source->value.battery.voltage_mv;
    break;
  case H2_GIZCLAW_TELEMETRY_GNSS:
    target->gnss.latitude = source->value.gnss.latitude;
    target->gnss.longitude = source->value.gnss.longitude;
    target->gnss.has_altitude_m = source->value.gnss.has_altitude_m;
    target->gnss.altitude_m = source->value.gnss.altitude_m;
    target->gnss.has_accuracy_m = source->value.gnss.has_accuracy_m;
    target->gnss.accuracy_m = source->value.gnss.accuracy_m;
    break;
  case H2_GIZCLAW_TELEMETRY_NETWORK:
    target->network.has_rssi_dbm = source->value.network.has_rssi_dbm;
    target->network.rssi_dbm = source->value.network.rssi_dbm;
    target->network.has_signal_level = source->value.network.has_signal_level;
    target->network.signal_level = source->value.network.signal_level;
    target->network.has_rat = source->value.network.has_rat;
    target->network.rat = to_gzc_str(source->value.network.rat);
    target->network.has_operator_name = source->value.network.has_operator_name;
    target->network.operator_name =
        to_gzc_str(source->value.network.operator_name);
    target->network.has_connected = source->value.network.has_connected;
    target->network.connected = source->value.network.connected;
    break;
  case H2_GIZCLAW_TELEMETRY_SYSTEM:
    target->system.has_uptime_seconds = source->value.system.has_uptime_seconds;
    target->system.uptime_seconds = source->value.system.uptime_seconds;
    target->system.has_free_memory_bytes =
        source->value.system.has_free_memory_bytes;
    target->system.free_memory_bytes = source->value.system.free_memory_bytes;
    target->system.has_temperature_c = source->value.system.has_temperature_c;
    target->system.temperature_c = source->value.system.temperature_c;
    target->system.has_firmware_version =
        source->value.system.has_firmware_version;
    target->system.firmware_version =
        to_gzc_str(source->value.system.firmware_version);
    target->system.has_software_version =
        source->value.system.has_software_version;
    target->system.software_version =
        to_gzc_str(source->value.system.software_version);
    target->system.has_hardware_version =
        source->value.system.has_hardware_version;
    target->system.hardware_version =
        to_gzc_str(source->value.system.hardware_version);
    break;
  default:
    break;
  }
}

static int result_from_gzc(int result) {
  switch (result) {
  case GZC_OK:
    return H2_PAL_OK;
  case GZC_ERR_INVALID_ARGUMENT:
    return H2_PAL_ERR_INVALID_ARG;
  case GZC_ERR_NO_MEMORY:
    return H2_PAL_ERR_NO_MEMORY;
  case GZC_ERR_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case GZC_ERR_CLOSED:
    return H2_PAL_ERR_CLOSED;
  case GZC_ERR_UNSUPPORTED:
    return H2_PAL_ERR_UNSUPPORTED;
  default:
    return H2_PAL_ERR_IO;
  }
}

static int telemetry_send_on_net(
    h2_gizclaw_client_t *client, const h2_gizclaw_telemetry_frame_t *frame) {
  if (client == NULL || frame == NULL || frame->sequence == 0u ||
      frame->observations == NULL || frame->observation_count == 0u ||
      frame->observation_count > H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  gzc_telemetry_observation_t
      observations[H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS];
  for (size_t index = 0u; index < frame->observation_count; ++index) {
    if (!observation_valid(&frame->observations[index]))
      return H2_PAL_ERR_INVALID_ARG;
    map_observation(&frame->observations[index], &observations[index]);
  }
  const gzc_telemetry_frame_t mapped = {
      .sequence = frame->sequence,
      .observed_at_unix_ms = frame->observed_at_unix_ms,
      .observations = observations,
      .observation_count = frame->observation_count,
  };
#if defined(H2_GIZCLAW_TESTING)
  if (test_send != NULL)
    return result_from_gzc(test_send(test_send_user, &mapped));
#endif
  gzc_client_t *gzc = h2_gizclaw_client_gzc_internal(client);
  if (gzc == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  return result_from_gzc(gzc_client_send_telemetry(gzc, &mapped));
}

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_client_telemetry_send(
    h2_gizclaw_client_t *client, const h2_gizclaw_telemetry_frame_t *frame) {
  return telemetry_send_on_net(client, frame);
}
#endif

struct h2_gizclaw_telemetry_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_telemetry_completion_fn completion;
  void *completion_user;
  h2_gizclaw_operation_result_t operation_result;
  h2_gizclaw_telemetry_frame_t frame;
  h2_gizclaw_telemetry_observation_t
      observations[H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS];
  char strings[H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS][3]
              [H2_GIZCLAW_TELEMETRY_VERSION_MAX + 1u];
  atomic_bool terminal;
};

static h2_pal_result_t telemetry_copy_span(char *target, size_t capacity,
                                           h2_gizclaw_str_t source,
                                           h2_gizclaw_str_t *out) {
  if (source.data == NULL || source.len == 0u || source.len >= capacity)
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(target, source.data, source.len);
  target[source.len] = '\0';
  *out = (h2_gizclaw_str_t){.data = target, .len = source.len};
  return H2_PAL_OK;
}

static h2_pal_result_t
telemetry_copy_frame(h2_gizclaw_telemetry_request_t *request,
                     const h2_gizclaw_telemetry_frame_t *frame) {
  if (frame == NULL || frame->sequence == 0u || frame->observations == NULL ||
      frame->observation_count == 0u ||
      frame->observation_count > H2_GIZCLAW_TELEMETRY_MAX_OBSERVATIONS)
    return H2_PAL_ERR_INVALID_ARG;
  request->frame = *frame;
  request->frame.observations = request->observations;
  memcpy(request->observations, frame->observations,
         frame->observation_count * sizeof(request->observations[0]));
  for (size_t index = 0u; index < frame->observation_count; ++index) {
    h2_gizclaw_telemetry_observation_t *observation =
        &request->observations[index];
    if (!observation_valid(observation))
      return H2_PAL_ERR_INVALID_ARG;
    if (observation->kind == H2_GIZCLAW_TELEMETRY_NETWORK) {
      if (observation->value.network.has_rat &&
          telemetry_copy_span(request->strings[index][0],
                              sizeof(request->strings[index][0]),
                              observation->value.network.rat,
                              &observation->value.network.rat) != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_ARG;
      if (observation->value.network.has_operator_name &&
          telemetry_copy_span(
              request->strings[index][1], sizeof(request->strings[index][1]),
              observation->value.network.operator_name,
              &observation->value.network.operator_name) != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_ARG;
    } else if (observation->kind == H2_GIZCLAW_TELEMETRY_SYSTEM) {
      const h2_gizclaw_str_t sources[3] = {
          observation->value.system.firmware_version,
          observation->value.system.software_version,
          observation->value.system.hardware_version,
      };
      h2_gizclaw_str_t *targets[3] = {
          &observation->value.system.firmware_version,
          &observation->value.system.software_version,
          &observation->value.system.hardware_version,
      };
      const bool present[3] = {
          observation->value.system.has_firmware_version,
          observation->value.system.has_software_version,
          observation->value.system.has_hardware_version,
      };
      for (size_t text = 0u; text < 3u; ++text) {
        if (present[text] &&
            telemetry_copy_span(request->strings[index][text],
                                sizeof(request->strings[index][text]),
                                sources[text], targets[text]) != H2_PAL_OK)
          return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t
telemetry_run(void *user, h2_gizclaw_client_t *client,
              const h2_gizclaw_cancel_token_t *cancel_token) {
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  h2_gizclaw_telemetry_request_t *request = user;
  return (h2_pal_result_t)telemetry_send_on_net(client, &request->frame);
}

static void telemetry_complete(void *user, h2_gizclaw_operation_t *operation,
                               const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_telemetry_request_t *request = user;
  request->operation_result = *result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

h2_pal_result_t h2_gizclaw_service_telemetry_send_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_telemetry_frame_t *frame,
    h2_gizclaw_telemetry_completion_fn completion, void *user,
    h2_gizclaw_telemetry_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_telemetry_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->completion = completion;
  request->completion_user = user;
  h2_pal_result_t rc = telemetry_copy_frame(request, frame);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_submit(service, identity, telemetry_run,
                                   telemetry_complete, request,
                                   &request->operation);
  }
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_telemetry_request_cancel(h2_gizclaw_telemetry_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

h2_pal_result_t h2_gizclaw_telemetry_request_wait(
    h2_gizclaw_telemetry_request_t *request, uint32_t timeout_ms) {
  return request == NULL
             ? H2_PAL_ERR_INVALID_ARG
             : h2_gizclaw_operation_wait(request->operation, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_telemetry_request_operation_result(
    const h2_gizclaw_telemetry_request_t *request) {
  return request != NULL &&
                 atomic_load_explicit(&request->terminal, memory_order_acquire)
             ? &request->operation_result
             : NULL;
}

void h2_gizclaw_telemetry_request_release(
    h2_gizclaw_telemetry_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_pal_mem_free(request->allocator, request);
}
