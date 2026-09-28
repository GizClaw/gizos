#include "h2_gizclaw_mhs_internal.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HWD_PAYLOAD_MAX 512u

static bool id_valid(const char *id) {
  if (id == NULL || id[0] < 'a' || id[0] > 'z')
    return false;
  bool separator = false;
  for (size_t i = 0; i <= H2_GIZCLAW_MHS_ID_MAX_BYTES; ++i) {
    const char c = id[i];
    if (c == '\0')
      return !separator;
    if (i == H2_GIZCLAW_MHS_ID_MAX_BYTES)
      return false;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      separator = false;
    else if ((c == '.' || c == '-') && !separator)
      separator = true;
    else
      return false;
  }
  return false;
}

static bool writable(gizclaw_rpc_v1_ClientHwd hwd) {
  return hwd == gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY ||
         hwd == gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED ||
         hwd == gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER;
}

int h2_gizclaw_mhs_validate_internal(const h2_gizclaw_mhs_device_t *devices,
                                     size_t count) {
  if ((count != 0u && devices == NULL) ||
      count > SIZE_MAX / sizeof(*devices))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < count; ++i) {
    const h2_gizclaw_mhs_device_t *device = &devices[i];
    if (!id_valid(device->id) || device->read == NULL ||
        device->hwd < gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI ||
        device->hwd > gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER ||
        (device->write != NULL && !writable(device->hwd)))
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0u; j < i; ++j)
      if (strcmp(device->id, devices[j].id) == 0)
        return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t speaker_snapshot(h2_gizclaw_mhs_builtin_t *builtin,
                                        h2_gizclaw_mhs_read_t *out) {
  h2_runtime_system_audio_state_t snapshot = {0};
  h2_pal_result_t rc =
      builtin->runtime != NULL && builtin->audio == builtin->runtime->audio
          ? h2_runtime_system_state_audio(builtin->runtime, &snapshot)
          : h2_pal_audio_get_speaker_volume_percent(
                builtin->audio, &snapshot.volume_percent);
  if (rc != H2_PAL_OK)
    return rc;
  if (builtin->runtime == NULL || builtin->audio != builtin->runtime->audio)
    snapshot.muted = snapshot.volume_percent == 0u;
  out->speaker.has_volume_percent = true;
  out->speaker.volume_percent = snapshot.volume_percent;
  out->speaker.has_muted = true;
  out->speaker.muted = snapshot.muted;
  return H2_PAL_OK;
}

static h2_pal_result_t speaker_read(void *user, h2_gizclaw_mhs_read_t *out) {
  return speaker_snapshot(user, out);
}

static h2_pal_result_t speaker_write(void *user,
                                     const h2_gizclaw_mhs_write_t *request,
                                     h2_gizclaw_mhs_read_t *out) {
  h2_gizclaw_mhs_builtin_t *builtin = user;
  h2_gizclaw_mhs_read_t previous = {0};
  h2_pal_result_t rc = speaker_snapshot(builtin, &previous);
  if (rc != H2_PAL_OK)
    return rc;
  const gizclaw_rpc_v1_SpeakerHwdWriteRequest *patch = &request->speaker;
  const uint32_t volume = patch->has_volume_percent
                              ? patch->volume_percent
                              : previous.speaker.volume_percent;
  const bool muted = patch->has_muted ? patch->muted : previous.speaker.muted;
  rc = builtin->runtime != NULL && builtin->audio == builtin->runtime->audio
           ? h2_runtime_audio_set_volume(builtin->runtime, volume, muted)
           : h2_pal_audio_set_speaker_volume_percent(
                 builtin->audio, muted ? 0u : volume);
  return rc == H2_PAL_OK ? speaker_snapshot(builtin, out) : rc;
}

static h2_pal_result_t wifi_read(void *user, h2_gizclaw_mhs_read_t *out) {
  h2_gizclaw_mhs_builtin_t *builtin = user;
  h2_pal_wifi_sta_status_t status = {0};
  h2_pal_result_t rc = h2_pal_wifi_sta_get_status(builtin->wifi, &status);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_WifiHwdReadResponse *value = &out->wifi;
  value->has_connected = true;
  value->connected = status.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
                     status.state == H2_PAL_WIFI_STA_STATE_GOT_IP;
  if (status.ssid_len != 0u && status.ssid_len <= 32u &&
      memchr(status.ssid, '\0', status.ssid_len) == NULL) {
    value->has_ssid = true;
    memcpy(value->ssid, status.ssid, status.ssid_len);
    value->ssid[status.ssid_len] = '\0';
  }
  /* RSSI is a measurement of the current association only. */
  value->has_rssi_dbm = value->connected;
  value->rssi_dbm = value->connected ? status.rssi : 0;
  if (status.ip_valid) {
    uint8_t ip[4];
    h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
    value->has_ip = true;
    (void)snprintf(value->ip, sizeof(value->ip), "%u.%u.%u.%u",
                   ip[0], ip[1], ip[2], ip[3]);
  }
  if (status.bssid_set) {
    const uint8_t *mac = status.bssid;
    value->has_bssid = true;
    (void)snprintf(value->bssid, sizeof(value->bssid),
                   "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                   mac[2], mac[3], mac[4], mac[5]);
  }
  return H2_PAL_OK;
}

size_t h2_gizclaw_mhs_builtins_internal(h2_gizclaw_mhs_builtin_t *context,
                                        h2_gizclaw_mhs_device_t *devices) {
  size_t count = 0u;
  if (context->audio != NULL && context->audio->vtable != NULL &&
      context->audio->vtable->get_speaker_volume_percent != NULL) {
    devices[count++] = (h2_gizclaw_mhs_device_t){
        .id = "speaker.main",
        .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER,
        .read = speaker_read,
        .write = context->audio->vtable->set_speaker_volume_percent != NULL
                     ? speaker_write
                     : NULL,
        .user = context};
  }
  if (context->wifi != NULL && context->wifi->vtable != NULL &&
      context->wifi->vtable->get_status != NULL) {
    devices[count++] = (h2_gizclaw_mhs_device_t){
        .id = "wifi.main",
        .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI,
        .read = wifi_read,
        .user = context};
  }
  return count;
}

typedef struct mhs_request {
  char id[H2_GIZCLAW_MHS_ID_MAX_BYTES + 1u];
  gizclaw_rpc_v1_ClientHwd hwd;
  uint8_t payload[HWD_PAYLOAD_MAX];
  size_t payload_len;
  bool id_seen, hwd_seen, payload_seen;
} mhs_request_t;

static bool decode_request(h2_gizclaw_rpc_bytes_t data, bool write,
                           mhs_request_t *out) {
  if (data.data == NULL || data.len == 0u)
    return false;
  pb_istream_t input = pb_istream_from_buffer(data.data, data.len);
  while (input.bytes_left != 0u) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof = false;
    if (!pb_decode_tag(&input, &wire, &tag, &eof))
      return false;
    if (tag == 1u) {
      pb_istream_t sub;
      if (out->id_seen || wire != PB_WT_STRING ||
          !pb_make_string_substream(&input, &sub) ||
          sub.bytes_left == 0u || sub.bytes_left > H2_GIZCLAW_MHS_ID_MAX_BYTES)
        return false;
      const size_t length = sub.bytes_left;
      if (!pb_read(&sub, (pb_byte_t *)out->id, length) ||
          !pb_close_string_substream(&input, &sub))
        return false;
      out->id[length] = '\0';
      out->id_seen = true;
    } else if (tag == 2u) {
      uint32_t hwd = 0u;
      if (out->hwd_seen || wire != PB_WT_VARINT ||
          !pb_decode_varint32(&input, &hwd))
        return false;
      out->hwd = (gizclaw_rpc_v1_ClientHwd)hwd;
      out->hwd_seen = true;
    } else if (tag == 3u && write) {
      pb_istream_t sub;
      if (out->payload_seen || wire != PB_WT_STRING ||
          !pb_make_string_substream(&input, &sub) ||
          sub.bytes_left == 0u || sub.bytes_left > sizeof(out->payload))
        return false;
      out->payload_len = sub.bytes_left;
      if (!pb_read(&sub, out->payload, out->payload_len) ||
          !pb_close_string_substream(&input, &sub))
        return false;
      out->payload_seen = true;
    } else if (!pb_skip_field(&input, wire))
      return false;
  }
  return out->id_seen && out->hwd_seen && id_valid(out->id) &&
         out->hwd >= gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI &&
         out->hwd <= gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER &&
         (!write || out->payload_seen);
}

static const pb_msgdesc_t *read_fields(gizclaw_rpc_v1_ClientHwd hwd) {
  switch (hwd) {
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI:
    return gizclaw_rpc_v1_WifiHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_BLE:
    return gizclaw_rpc_v1_BleHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MODEM:
    return gizclaw_rpc_v1_ModemHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_BATTERY:
    return gizclaw_rpc_v1_BatteryHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MIC:
    return gizclaw_rpc_v1_MicHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY:
    return gizclaw_rpc_v1_DisplayHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED:
    return gizclaw_rpc_v1_LedHwdReadResponse_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER:
    return gizclaw_rpc_v1_SpeakerHwdReadResponse_fields;
  default:
    return NULL;
  }
}

static const pb_msgdesc_t *write_fields(gizclaw_rpc_v1_ClientHwd hwd) {
  switch (hwd) {
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY:
    return gizclaw_rpc_v1_DisplayHwdWriteRequest_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED:
    return gizclaw_rpc_v1_LedHwdWriteRequest_fields;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER:
    return gizclaw_rpc_v1_SpeakerHwdWriteRequest_fields;
  default:
    return NULL;
  }
}

static bool write_valid(gizclaw_rpc_v1_ClientHwd hwd,
                        const h2_gizclaw_mhs_write_t *request) {
  switch (hwd) {
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY:
    return (request->display.has_brightness_percent ||
            request->display.has_enabled ||
            request->display.has_off_timeout_ms) &&
           (!request->display.has_brightness_percent ||
            request->display.brightness_percent <= 100u);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED:
    return (request->led.has_enabled ||
            request->led.has_brightness_percent) &&
           (!request->led.has_brightness_percent ||
            request->led.brightness_percent <= 100u);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER:
    return (request->speaker.has_volume_percent ||
            request->speaker.has_muted) &&
           (!request->speaker.has_volume_percent ||
            request->speaker.volume_percent <= 100u);
  default:
    return false;
  }
}

static bool read_valid(gizclaw_rpc_v1_ClientHwd hwd,
                       const h2_gizclaw_mhs_read_t *value) {
  switch (hwd) {
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI:
    return (value->wifi.has_connected || value->wifi.has_ssid ||
            value->wifi.has_bssid || value->wifi.has_rssi_dbm ||
            value->wifi.has_ip) &&
           (!value->wifi.has_ssid ||
            memchr(value->wifi.ssid, '\0', sizeof(value->wifi.ssid)) != NULL) &&
           (!value->wifi.has_bssid ||
            memchr(value->wifi.bssid, '\0', sizeof(value->wifi.bssid)) != NULL) &&
           (!value->wifi.has_ip ||
            memchr(value->wifi.ip, '\0', sizeof(value->wifi.ip)) != NULL);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_BLE:
    return value->ble.has_powered || value->ble.has_advertising ||
           value->ble.has_scanning || value->ble.has_connection_count;
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MODEM:
    return (value->modem.has_sim_present || value->modem.has_registered ||
            value->modem.has_rat || value->modem.has_rssi_dbm ||
            value->modem.has_signal_level) &&
           (!value->modem.has_rat ||
            memchr(value->modem.rat, '\0', sizeof(value->modem.rat)) != NULL);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_BATTERY:
    return (value->battery.has_percent || value->battery.has_charging ||
            value->battery.has_voltage_mv) &&
           (!value->battery.has_percent ||
            (isfinite(value->battery.percent) &&
             value->battery.percent >= 0.0 &&
             value->battery.percent <= 100.0)) &&
           (!value->battery.has_voltage_mv ||
            (isfinite(value->battery.voltage_mv) &&
             value->battery.voltage_mv >= 0.0));
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY:
    return (value->display.has_brightness_percent ||
            value->display.has_enabled ||
            value->display.has_off_timeout_ms) &&
           (!value->display.has_brightness_percent ||
            value->display.brightness_percent <= 100u);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED:
    return (value->led.has_enabled || value->led.has_brightness_percent) &&
           (!value->led.has_brightness_percent ||
            value->led.brightness_percent <= 100u);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER:
    return (value->speaker.has_volume_percent || value->speaker.has_muted) &&
           (!value->speaker.has_volume_percent ||
            value->speaker.volume_percent <= 100u);
  case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_MIC:
    return value->mic.has_available || value->mic.has_capturing;
  default:
    return false;
  }
}

int h2_gizclaw_mhs_request_internal(
    const h2_gizclaw_mhs_device_t *devices, size_t count, bool write,
    const h2_pal_mem_api_t *allocator, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response, uint8_t **storage) {
  if (response == NULL || storage == NULL || allocator == NULL ||
      (count != 0u && devices == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  *storage = NULL;
  memset(response, 0, sizeof(*response));
  mhs_request_t decoded = {0};
  if (!decode_request(request, write, &decoded))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_gizclaw_mhs_device_t *device = NULL;
  for (size_t i = 0u; i < count; ++i)
    if (strcmp(devices[i].id, decoded.id) == 0) {
      device = &devices[i];
      break;
    }
  if (device == NULL)
    return H2_PAL_ERR_NOT_FOUND;
  if (decoded.hwd != device->hwd)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_mhs_read_t applied = {0};
  if (write) {
    const pb_msgdesc_t *fields = write_fields(decoded.hwd);
    if (fields == NULL || device->write == NULL)
      return H2_PAL_ERR_UNSUPPORTED;
    h2_gizclaw_mhs_write_t patch = {0};
    pb_istream_t input = pb_istream_from_buffer(decoded.payload,
                                                 decoded.payload_len);
    if (!pb_decode(&input, fields, &patch) ||
        !write_valid(decoded.hwd, &patch))
      return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t rc = device->write(device->user, &patch, &applied);
    if (rc != H2_PAL_OK)
      return rc;
  } else {
    h2_pal_result_t rc = device->read(device->user, &applied);
    if (rc != H2_PAL_OK)
      return rc;
  }
  if (!read_valid(decoded.hwd, &applied))
    return H2_PAL_ERR_IO;
  const pb_msgdesc_t *fields = read_fields(decoded.hwd);
  size_t inner_size = 0u;
  uint8_t inner[HWD_PAYLOAD_MAX];
  if (fields == NULL || !pb_get_encoded_size(&inner_size, fields, &applied) ||
      inner_size > sizeof(inner))
    return H2_PAL_ERR_IO;
  pb_ostream_t inner_out = pb_ostream_from_buffer(inner, inner_size);
  if (!pb_encode(&inner_out, fields, &applied))
    return H2_PAL_ERR_IO;
  if (write) {
    const pb_msgdesc_t *reply_fields = NULL;
    uint8_t nested[HWD_PAYLOAD_MAX];
    size_t nested_size = 0u;
    switch (decoded.hwd) {
    case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_DISPLAY: {
      gizclaw_rpc_v1_DisplayHwdWriteResponse reply = {
          .has_applied = true, .applied = applied.display};
      reply_fields = gizclaw_rpc_v1_DisplayHwdWriteResponse_fields;
      if (!pb_get_encoded_size(&nested_size, reply_fields, &reply) ||
          nested_size > sizeof(nested))
        return H2_PAL_ERR_IO;
      pb_ostream_t stream = pb_ostream_from_buffer(nested, nested_size);
      if (!pb_encode(&stream, reply_fields, &reply))
        return H2_PAL_ERR_IO;
      break;
    }
    case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED: {
      gizclaw_rpc_v1_LedHwdWriteResponse reply = {
          .has_applied = true, .applied = applied.led};
      reply_fields = gizclaw_rpc_v1_LedHwdWriteResponse_fields;
      if (!pb_get_encoded_size(&nested_size, reply_fields, &reply) ||
          nested_size > sizeof(nested))
        return H2_PAL_ERR_IO;
      pb_ostream_t stream = pb_ostream_from_buffer(nested, nested_size);
      if (!pb_encode(&stream, reply_fields, &reply))
        return H2_PAL_ERR_IO;
      break;
    }
    case gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_SPEAKER: {
      gizclaw_rpc_v1_SpeakerHwdWriteResponse reply = {
          .has_applied = true, .applied = applied.speaker};
      reply_fields = gizclaw_rpc_v1_SpeakerHwdWriteResponse_fields;
      if (!pb_get_encoded_size(&nested_size, reply_fields, &reply) ||
          nested_size > sizeof(nested))
        return H2_PAL_ERR_IO;
      pb_ostream_t stream = pb_ostream_from_buffer(nested, nested_size);
      if (!pb_encode(&stream, reply_fields, &reply))
        return H2_PAL_ERR_IO;
      break;
    }
    default:
      return H2_PAL_ERR_UNSUPPORTED;
    }
    memcpy(inner, nested, nested_size);
    inner_size = nested_size;
  }
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode_tag(&sizing, PB_WT_STRING, 1u) ||
      !pb_encode_string(&sizing, inner, inner_size))
    return H2_PAL_ERR_IO;
  *storage = h2_pal_mem_alloc(allocator, sizing.bytes_written);
  if (*storage == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(*storage, sizing.bytes_written);
  if (!pb_encode_tag(&output, PB_WT_STRING, 1u) ||
      !pb_encode_string(&output, inner, inner_size)) {
    h2_pal_mem_free(allocator, *storage);
    *storage = NULL;
    return H2_PAL_ERR_IO;
  }
  response->payload = (h2_gizclaw_rpc_bytes_t){*storage,
                                                 output.bytes_written};
  return H2_PAL_OK;
}

int h2_gizclaw_mhs_error_internal(int result) {
  switch (result) {
  case H2_PAL_ERR_NOT_FOUND:
    return H2_GIZCLAW_RPC_ERROR_NOT_FOUND;
  case H2_PAL_ERR_INVALID_ARG:
  case H2_PAL_ERR_FORMAT:
    return H2_GIZCLAW_RPC_ERROR_INVALID_ARGUMENT;
  case H2_PAL_ERR_INVALID_STATE:
    return H2_GIZCLAW_RPC_ERROR_FAILED_PRECONDITION;
  case H2_PAL_ERR_UNSUPPORTED:
    return H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED;
  case H2_PAL_ERR_NO_MEMORY:
  case H2_PAL_ERR_BUSY:
    return H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED;
  case H2_PAL_ERR_TIMEOUT:
    return H2_GIZCLAW_RPC_ERROR_DEADLINE_EXCEEDED;
  /* A device that has not published its state yet is retryable, like a
   * closing client; neither is an internal fault. */
  case H2_PAL_ERR_UNAVAILABLE:
  case H2_PAL_ERR_CLOSED:
    return H2_GIZCLAW_RPC_ERROR_UNAVAILABLE;
  default:
    return H2_GIZCLAW_RPC_ERROR_INTERNAL;
  }
}
