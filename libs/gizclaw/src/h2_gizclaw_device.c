#include "h2_gizclaw_device_internal.h"
#include "h2_runtime.h"
#include "h2_gizclaw_firmware.h"
#include "h2_gizclaw_ogg_opus_internal.h"
#include "h2_gizclaw_ota.h"
#include "h2_gizclaw_player.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_task_names.h"
#include "h2_gizclaw_telemetry.h"
#include "payload/audioplayer.pb.h"
#include "payload/firmware.pb.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* PAL HTTP bounds the entire streamed transfer, including Stage writes.
 * Firmware packages need a separate budget from short connection/RPC waits. */
#define OTA_DOWNLOAD_TIMEOUT_MS 600000

/* RPC dispatch is serialized by the client. Only snapshots/commands cross the
 * mutex; HTTP, PCM, telemetry, and backend actions never run under it. */
typedef struct audio_download audio_download_t;
struct h2_gizclaw_device {
  h2_gizclaw_config_t config;
  h2_gizclaw_service_t *service;
  h2_pal_mutex_t *mutex;
  h2_pal_task_t *task;
  audio_download_t *download;
  atomic_bool stopping;
  atomic_uint generation;
  uint32_t worker_generation, sequence;
  bool playing, dirty;
  gizclaw_rpc_v1_AudioPlayerStatus status;
  gizclaw_rpc_v1_ClientDeviceAudioPlayerPlaylistGetResponse *playlist;
  /* Large generated messages are heap-owned, not task stack allocations. */
  gizclaw_rpc_v1_ClientDeviceAudioPlayerPlaylistSetRequest *incoming;
  uint8_t *response;
  size_t response_capacity;
  int pending;
  bool pending_ready;
  uint32_t pending_generation;
  uint32_t delay_ms;
  char sound[33];
  uint32_t sound_ms;
  gizclaw_rpc_v1_ClientFirmwareUpdateRequest update;
  h2_gizclaw_ota_status_t ota_status;
  h2_pal_wifi_sta_config_t wifi_config;
};

static void trace(h2_gizclaw_device_t *d, const char *stage, int method,
                  int result) {
  char message[128];
  (void)snprintf(message, sizeof(message), "device stage=%s method=%d rc=%d",
                 stage, method, result);
  (void)h2_pal_log_write(
      d->config.log, result == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN,
      "gizclaw", message);
}
static void lock(h2_gizclaw_device_t *d) {
  (void)h2_pal_mutex_lock(d->service->config.sync, d->mutex);
}
static void unlock(h2_gizclaw_device_t *d) {
  (void)h2_pal_mutex_unlock(d->service->config.sync, d->mutex);
}
static h2_gizclaw_str_t text_span(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
static bool decode(h2_gizclaw_rpc_bytes_t bytes, const pb_msgdesc_t *fields,
                   void *message) {
  pb_istream_t input = pb_istream_from_buffer(bytes.data, bytes.len);
  return pb_decode(&input, fields, message);
}
static int encode(h2_gizclaw_device_t *d, const pb_msgdesc_t *fields,
                  const void *message,
                  h2_gizclaw_rpc_provider_response_t *out) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, message))
    return H2_PAL_ERR_FORMAT;
  if (size > d->response_capacity) {
    uint8_t *buffer = h2_pal_mem_alloc(d->config.allocator, size);
    if (!buffer)
      return H2_PAL_ERR_NO_MEMORY;
    h2_pal_mem_free(d->config.allocator, d->response);
    d->response = buffer;
    d->response_capacity = size;
  }
  pb_ostream_t output =
      pb_ostream_from_buffer(d->response, d->response_capacity);
  if (!pb_encode(&output, fields, message))
    return H2_PAL_ERR_FORMAT;
  out->payload = (h2_gizclaw_rpc_bytes_t){d->response, output.bytes_written};
  return H2_PAL_OK;
}
static int rpc_error(h2_gizclaw_rpc_provider_response_t *out, int code,
                     const char *message) {
  out->has_error = true;
  out->error_code = code;
  out->error_message =
      (h2_gizclaw_rpc_bytes_t){(const uint8_t *)message, strlen(message)};
  return H2_PAL_OK;
}
static bool string_encode(pb_ostream_t *stream, const pb_field_t *field,
                          void *const *arg) {
  const char *s = *arg;
  return s == NULL ||
         (pb_encode_tag_for_field(stream, field) &&
          pb_encode_string(stream, (const pb_byte_t *)s, strlen(s)));
}
static pb_callback_t string_field(const char *s) {
  return (pb_callback_t){.funcs.encode = string_encode, .arg = (void *)s};
}
/* IMEI is personal data: it is encoded into the response only, never traced
 * or logged. Products must supply exactly 15 ASCII decimal digits. */
static bool imei_valid(const h2_gizclaw_device_imei_t *imei) {
  for (size_t i = 0; i < 15u; ++i) {
    if (imei->digits[i] < '0' || imei->digits[i] > '9')
      return false;
  }
  return imei->digits[15] == '\0' &&
         (!imei->name || strlen(imei->name) <= H2_GIZCLAW_DEVICE_IMEI_NAME_MAX);
}
static bool imei_encode(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const h2_gizclaw_device_facts_t *facts = *arg;
  for (size_t i = 0; i < facts->imei_count; ++i) {
    char tac[9], serial[8];
    memcpy(tac, facts->imeis[i].digits, 8u);
    tac[8] = '\0';
    memcpy(serial, facts->imeis[i].digits + 8, 7u);
    serial[7] = '\0';
    gizclaw_rpc_v1_PeerIMEI item = gizclaw_rpc_v1_PeerIMEI_init_zero;
    item.name = string_field(facts->imeis[i].name);
    item.tac = string_field(tac);
    item.serial = string_field(serial);
    if (!pb_encode_tag_for_field(stream, field) ||
        !pb_encode_submessage(stream, gizclaw_rpc_v1_PeerIMEI_fields, &item))
      return false;
  }
  return true;
}
static bool https_url(const char *s) {
  if (strncmp(s, "https://", 8) != 0 || !s[8] || s[8] == '/')
    return false;
  const char *end = s + 8 + strcspn(s + 8, "/?#");
  if (end == s + 8)
    return false;
  for (const char *p = s; *p; ++p) {
    if ((unsigned char)*p <= 32 || *p == '\\' || (p < end && *p == '@'))
      return false;
  }
  return true;
}
static bool sha_valid(const char *s) {
  if (strlen(s) != 64)
    return false;
  return strspn(s, "0123456789abcdefABCDEF") == 64;
}
static bool sha_equal(const char *a, const char *b) {
  for (size_t i = 0; i < 64; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'F')
      x += 'a' - 'A';
    if (y >= 'A' && y <= 'F')
      y += 'a' - 'A';
    if (x != y)
      return false;
  }
  return true;
}
static void changed(h2_gizclaw_device_t *d) { d->dirty = true; }
static int player_reply(h2_gizclaw_device_t *d,
                        h2_gizclaw_rpc_provider_response_t *out) {
  gizclaw_rpc_v1_ClientDeviceAudioPlayerGetResponse reply = {0};
  lock(d);
  reply.has_value = true;
  reply.value = d->status;
  unlock(d);
  /* All player mutations have the same field-1 status envelope. */
  return encode(d, gizclaw_rpc_v1_ClientDeviceAudioPlayerGetResponse_fields,
                &reply, out);
}
static void cancel_play_locked(h2_gizclaw_device_t *d) {
  d->playing = false;
  atomic_fetch_add(&d->generation, 1u);
  strcpy(d->status.state, "stopped");
  changed(d);
}
static void response_complete(void *user, int result);
static int player_rpc(h2_gizclaw_device_t *d, int method,
                      h2_gizclaw_rpc_bytes_t bytes,
                      h2_gizclaw_rpc_provider_response_t *out) {
  if (d->config.audio == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  const int base = H2_GIZCLAW_RPC_CLIENT_DEVICE_AUDIOPLAYER_GET;
  if (method == base || method == base + 1 || method == base + 5) {
    gizclaw_rpc_v1_ClientDeviceAudioPlayerGetRequest empty = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientDeviceAudioPlayerGetRequest_fields,
                &empty))
      return H2_PAL_ERR_INVALID_ARG;
    if (method == base + 1) {
      lock(d);
      int rc = encode(
          d, gizclaw_rpc_v1_ClientDeviceAudioPlayerPlaylistGetResponse_fields,
          d->playlist, out);
      unlock(d);
      return rc;
    }
    if (method == base + 5) {
      int rc = h2_gizclaw_player_stop(d->service);
      if (rc != H2_PAL_OK)
        return rc;
    }
  } else if (method == base + 2 || method == base + 3) {
    memset(d->incoming, 0, sizeof(*d->incoming));
    if (!decode(bytes,
                gizclaw_rpc_v1_ClientDeviceAudioPlayerPlaylistSetRequest_fields,
                d->incoming))
      return H2_PAL_ERR_INVALID_ARG;
    bool append = method == base + 3;
    if (append && d->incoming->items_count == 0)
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0; i < d->incoming->items_count; ++i)
      if (!https_url(d->incoming->items[i].url))
        return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    size_t offset = append ? d->playlist->items_count : 0;
    if (offset + d->incoming->items_count > 32) {
      unlock(d);
      return H2_PAL_ERR_BUSY;
    }
    if (!append) {
      if (d->pending &&
          d->pending != H2_GIZCLAW_RPC_CLIENT_DEVICE_AUDIOPLAYER_PLAY) {
        unlock(d);
        return H2_PAL_ERR_BUSY;
      }
      cancel_play_locked(d);
      d->status.has_current_index = false;
      d->status.position_ms = 0;
      d->status.has_duration_ms = false;
      d->status.has_error_code = d->status.has_error_message = false;
    }
    memcpy(d->playlist->items + offset, d->incoming->items,
           d->incoming->items_count * sizeof(d->incoming->items[0]));
    d->playlist->items_count = (pb_size_t)(offset + d->incoming->items_count);
    ++d->playlist->playlist_revision;
    d->status.playlist_revision = d->playlist->playlist_revision;
    d->status.playlist_length = d->playlist->items_count;
    changed(d);
    unlock(d);
  } else if (method == base + 4) {
    gizclaw_rpc_v1_ClientDeviceAudioPlayerPlayRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientDeviceAudioPlayerPlayRequest_fields,
                &request))
      return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    uint32_t index = request.has_index ? request.index : 0;
    if (index >= d->playlist->items_count) {
      unlock(d);
      return H2_PAL_ERR_INVALID_ARG;
    }
    if (d->pending != 0) {
      unlock(d);
      return H2_PAL_ERR_BUSY;
    }
    atomic_fetch_add(&d->generation, 1u);
    d->playing = false;
    d->pending = H2_GIZCLAW_RPC_CLIENT_DEVICE_AUDIOPLAYER_PLAY;
    d->pending_generation = atomic_load(&d->generation);
    out->on_complete = response_complete;
    out->complete_user = d;
    d->status.has_current_index = true;
    d->status.current_index = index;
    d->status.position_ms = 0;
    d->status.has_duration_ms = false;
    d->status.has_error_code = d->status.has_error_message = false;
    strcpy(d->status.state, "buffering");
    changed(d);
    unlock(d);
  } else if (method == base + 6) {
    gizclaw_rpc_v1_ClientDeviceAudioPlayerModeSetRequest request = {0};
    if (!decode(bytes,
                gizclaw_rpc_v1_ClientDeviceAudioPlayerModeSetRequest_fields,
                &request) ||
        (strcmp(request.repeat, "off") && strcmp(request.repeat, "one") &&
         strcmp(request.repeat, "all")))
      return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    strcpy(d->status.repeat, request.repeat);
    changed(d);
    unlock(d);
  } else
    return H2_PAL_ERR_NOT_FOUND;
  return player_reply(d, out);
}

static void response_complete(void *user, int result) {
  h2_gizclaw_device_t *d = user;
  lock(d);
  trace(d, "response-complete", d->pending, result);
  if (d->pending == H2_GIZCLAW_RPC_CLIENT_DEVICE_AUDIOPLAYER_PLAY) {
    if (result == H2_PAL_OK && !atomic_load(&d->stopping) &&
        d->pending_generation == atomic_load(&d->generation)) {
      d->playing = true;
    } else if (d->pending_generation == atomic_load(&d->generation)) {
      cancel_play_locked(d);
    }
    d->pending = 0;
    unlock(d);
    return;
  }
  if (result == H2_PAL_OK && !atomic_load(&d->stopping)) {
    d->pending_ready = true;
    cancel_play_locked(d);
  } else
    d->pending = 0;
  unlock(d);
}
static int reserve_action(h2_gizclaw_device_t *d, int method,
                          h2_gizclaw_rpc_provider_response_t *out) {
  /* Called under mutex after validating and before committing parameters. */
  if (d->pending != 0)
    return H2_PAL_ERR_BUSY;
  d->pending = method;
  d->pending_ready = false;
  out->on_complete = response_complete;
  out->complete_user = d;
  return H2_PAL_OK;
}

static int status_reply(h2_gizclaw_device_t *d,
                        h2_gizclaw_rpc_provider_response_t *out) {
  gizclaw_rpc_v1_ClientDeviceStatusGetResponse reply = {0};
  h2_gizclaw_device_facts_t facts = {0};
  if (d->config.vtable && d->config.vtable->get_facts) {
    int rc = d->config.vtable->get_facts(d->config.user, &facts);
    if (rc != H2_PAL_OK)
      return rc;
    if ((facts.has_battery_percent &&
         (facts.battery_percent < 0 || facts.battery_percent > 100)) ||
        (facts.has_firmware_sha256 &&
         (facts.firmware_sha256[64] || !sha_valid(facts.firmware_sha256))))
      return H2_PAL_ERR_FORMAT;
  }
  reply.has_value = true;
  reply.value.has_battery_percent = facts.has_battery_percent;
  reply.value.battery_percent = facts.battery_percent;
  reply.value.has_charging = facts.has_charging;
  reply.value.charging = facts.charging;
  reply.value.has_firmware_sha256 = facts.has_firmware_sha256;
  memcpy(reply.value.firmware_sha256, facts.firmware_sha256, 65);
  h2_runtime_system_audio_state_t audio_state = {0};
  h2_runtime_t *runtime = d->service->config.runtime;
  int audio_rc;
  if (runtime && d->config.audio == runtime->audio) {
    audio_rc = h2_runtime_system_state_audio(runtime, &audio_state);
  } else {
    audio_rc = h2_pal_audio_get_speaker_volume_percent(
        d->config.audio, &audio_state.volume_percent);
    audio_state.muted = audio_state.volume_percent == 0;
  }
  if (audio_rc == H2_PAL_OK) {
    reply.value.has_volume = true;
    reply.value.volume = audio_state.volume_percent;
    reply.value.has_muted = true;
    reply.value.muted = audio_state.muted;
  }
  lock(d);
  reply.value.has_audioplayer = d->config.audio != NULL;
  reply.value.audioplayer = d->status;
  unlock(d);
  return encode(d, gizclaw_rpc_v1_ClientDeviceStatusGetResponse_fields, &reply,
                out);
}
static void bssid_text(char out[18], const uint8_t mac[6]) {
  (void)snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
}
static bool scan_result(void *user, const h2_pal_wifi_scan_entry_t *entry) {
  gizclaw_rpc_v1_ClientWifiScanResponse *reply = user;
  if (reply->networks_count >= 32 || entry->ssid_len > 32)
    return false;
  gizclaw_rpc_v1_WifiScanResult *network =
      &reply->networks[reply->networks_count++];
  memcpy(network->ssid, entry->ssid, entry->ssid_len);
  network->has_bssid = true;
  bssid_text(network->bssid, entry->bssid);
  network->has_rssi_dbm = true;
  network->rssi_dbm = entry->rssi;
  if (entry->channel >= 1 && entry->channel <= 14) {
    network->has_frequency_mhz = true;
    network->frequency_mhz =
        entry->channel == 14 ? 2484 : 2407 + entry->channel * 5;
  }
  const char *security = NULL;
  switch (entry->security) {
  case H2_PAL_WIFI_SECURITY_OPEN:
    security = "open";
    break;
  case H2_PAL_WIFI_SECURITY_WEP:
    security = "wep";
    break;
  case H2_PAL_WIFI_SECURITY_WPA:
    security = "wpa";
    break;
  case H2_PAL_WIFI_SECURITY_WPA2:
    security = "wpa2";
    break;
  case H2_PAL_WIFI_SECURITY_WPA3:
    security = "wpa3";
    break;
  default:
    break;
  }
  if (security) {
    network->has_security = true;
    strcpy(network->security, security);
  }
  return reply->networks_count < 32;
}
static int wifi_rpc(h2_gizclaw_device_t *d, int method,
                    h2_gizclaw_rpc_bytes_t bytes,
                    h2_gizclaw_rpc_provider_response_t *out) {
  if ((method == H2_GIZCLAW_RPC_CLIENT_WIFI_STATUS_GET ||
       method == H2_GIZCLAW_RPC_CLIENT_WIFI_SCAN) &&
      !d->config.wifi)
    return H2_PAL_ERR_UNSUPPORTED;
  if ((method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_LIST ||
       method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_FORGET) &&
      !d->config.wifi_settings)
    return H2_PAL_ERR_UNSUPPORTED;
  if (method == H2_GIZCLAW_RPC_CLIENT_WIFI_STATUS_GET) {
    gizclaw_rpc_v1_ClientWifiStatusGetRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientWifiStatusGetRequest_fields,
                &request))
      return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_status_t status = {0};
    int rc = h2_pal_wifi_sta_get_status(d->config.wifi, &status);
    if (rc != H2_PAL_OK)
      return rc;
    if (status.ssid_len > 32)
      return H2_PAL_ERR_FORMAT;
    gizclaw_rpc_v1_ClientWifiStatusGetResponse reply = {.has_value = true};
    reply.value.connected = status.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
                            status.state == H2_PAL_WIFI_STA_STATE_GOT_IP;
    reply.value.has_ssid = status.ssid_len != 0;
    memcpy(reply.value.ssid, status.ssid, status.ssid_len);
    reply.value.has_rssi_dbm = reply.value.connected;
    reply.value.rssi_dbm = status.rssi;
    reply.value.has_bssid = status.bssid_set;
    bssid_text(reply.value.bssid, status.bssid);
    reply.value.has_ip = status.ip_valid;
    uint8_t ip[4];
    h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
    (void)snprintf(reply.value.ip, sizeof(reply.value.ip), "%u.%u.%u.%u", ip[0],
                   ip[1], ip[2], ip[3]);
    return encode(d, gizclaw_rpc_v1_ClientWifiStatusGetResponse_fields, &reply,
                  out);
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_LIST ||
      method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_FORGET) {
    gizclaw_rpc_v1_ClientWifiSavedForgetRequest request = {0};
    const pb_msgdesc_t *fields =
        method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_LIST
            ? gizclaw_rpc_v1_ClientWifiSavedListRequest_fields
            : gizclaw_rpc_v1_ClientWifiSavedForgetRequest_fields;
    if (!decode(bytes, fields, &request))
      return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_config_t saved = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(d->config.wifi_settings,
                                                       &saved);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
      return rc;
    if (saved.ssid_len > 32)
      return H2_PAL_ERR_FORMAT;
    if (method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_LIST) {
      gizclaw_rpc_v1_ClientWifiSavedListResponse reply = {0};
      if (rc == H2_PAL_OK && saved.ssid_len) {
        reply.networks_count = 1;
        memcpy(reply.networks[0].ssid, saved.ssid, saved.ssid_len);
      }
      memset(&saved, 0, sizeof(saved));
      return encode(d, gizclaw_rpc_v1_ClientWifiSavedListResponse_fields,
                    &reply, out);
    }
    bool match = request.ssid[0] && strlen(request.ssid) == saved.ssid_len &&
                 !memcmp(request.ssid, saved.ssid, saved.ssid_len);
    memset(&saved, 0, sizeof(saved));
    if (!request.ssid[0])
      return H2_PAL_ERR_INVALID_ARG;
    return match ? h2_pal_wifi_settings_clear_saved_sta_config(
                       d->config.wifi_settings)
                 : H2_PAL_ERR_NOT_FOUND;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_WIFI_SCAN) {
    gizclaw_rpc_v1_ClientWifiScanRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientWifiScanRequest_fields, &request) ||
        (request.has_timeout_ms &&
         (request.timeout_ms <= 0 || request.timeout_ms > 30000)))
      return H2_PAL_ERR_INVALID_ARG;
    gizclaw_rpc_v1_ClientWifiScanResponse reply = {0};
    const h2_pal_wifi_scan_request_t scan = {0};
    int rc = h2_pal_wifi_sta_scan(
        d->config.wifi, &scan, scan_result, &reply,
        request.has_timeout_ms ? (uint32_t)request.timeout_ms : 5000u);
    return rc == H2_PAL_OK
               ? encode(d, gizclaw_rpc_v1_ClientWifiScanResponse_fields, &reply,
                        out)
               : rc;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_WIFI_CONNECT) {
    if (!d->config.wifi || !d->config.wifi->vtable ||
        !d->config.wifi->vtable->connect)
      return H2_PAL_ERR_UNSUPPORTED;
    gizclaw_rpc_v1_ClientWifiConnectRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientWifiConnectRequest_fields,
                &request) ||
        !request.ssid[0])
      return H2_PAL_ERR_INVALID_ARG;
    size_t password_len =
        request.has_passphrase ? strlen(request.passphrase) : 0;
    if (password_len && password_len < 8)
      return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    int rc = reserve_action(d, method, out);
    if (rc == H2_PAL_OK) {
      memset(&d->wifi_config, 0, sizeof(d->wifi_config));
      d->wifi_config.ssid_len = strlen(request.ssid);
      memcpy(d->wifi_config.ssid, request.ssid, d->wifi_config.ssid_len);
      d->wifi_config.password_len = password_len;
      memcpy(d->wifi_config.password, request.passphrase, password_len);
    }
    unlock(d);
    memset(&request, 0, sizeof(request));
    return rc;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static int device_rpc(h2_gizclaw_device_t *d, int method,
                      h2_gizclaw_rpc_bytes_t bytes,
                      h2_gizclaw_rpc_provider_response_t *out) {
  if (method >= 113 && method <= 119)
    return player_rpc(d, method, bytes, out);
  if ((method >= 104 && method <= 106) || method == 108 || method == 109)
    return wifi_rpc(d, method, bytes, out);
  if (method == H2_GIZCLAW_RPC_CLIENT_INFO_GET ||
      method == H2_GIZCLAW_RPC_CLIENT_IDENTIFIERS_GET ||
      method == H2_GIZCLAW_RPC_CLIENT_DEVICE_STATUS_GET) {
    gizclaw_rpc_v1_ClientGetInfoRequest empty = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientGetInfoRequest_fields, &empty))
      return H2_PAL_ERR_INVALID_ARG;
    if (method == H2_GIZCLAW_RPC_CLIENT_DEVICE_STATUS_GET)
      return status_reply(d, out);
    if (method == H2_GIZCLAW_RPC_CLIENT_INFO_GET) {
      gizclaw_rpc_v1_ClientGetInfoResponse reply = {.has_value = true};
      reply.value.manufacturer = string_field(d->config.manufacturer);
      reply.value.model = string_field(d->config.model);
      reply.value.hardware_revision = string_field(d->config.hardware_revision);
      return encode(d, gizclaw_rpc_v1_ClientGetInfoResponse_fields, &reply,
                    out);
    }
    gizclaw_rpc_v1_ClientGetIdentifiersResponse reply = {.has_value = true};
    reply.value.sn = string_field(d->config.serial);
    /* Absent vtable or a failing get_facts degrades to sn only; malformed
     * IMEIs fail the whole reply rather than sending a partial list. */
    h2_gizclaw_device_facts_t facts = {0};
    if (d->config.vtable && d->config.vtable->get_facts &&
        d->config.vtable->get_facts(d->config.user, &facts) == H2_PAL_OK &&
        facts.imei_count) {
      if (facts.imei_count > H2_GIZCLAW_DEVICE_IMEI_MAX)
        return H2_PAL_ERR_FORMAT;
      for (size_t i = 0; i < facts.imei_count; ++i) {
        if (!imei_valid(&facts.imeis[i]))
          return H2_PAL_ERR_FORMAT;
      }
      reply.value.imeis =
          (pb_callback_t){.funcs.encode = imei_encode, .arg = &facts};
    }
    return encode(d, gizclaw_rpc_v1_ClientGetIdentifiersResponse_fields, &reply,
                  out);
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_DEVICE_VOLUME_SET) {
    if (!d->config.audio || !d->config.audio->vtable ||
        !d->config.audio->vtable->set_speaker_volume_percent)
      return H2_PAL_ERR_UNSUPPORTED;
    gizclaw_rpc_v1_ClientDeviceVolumeSetRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientDeviceVolumeSetRequest_fields,
                &request) ||
        request.level < 0 || request.level > 100)
      return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_t *runtime = d->service->config.runtime;
    int rc = runtime && d->config.audio == runtime->audio
        ? h2_runtime_audio_set_volume(runtime, (uint32_t)request.level, request.muted)
        : h2_pal_audio_set_speaker_volume_percent(
              d->config.audio, request.muted ? 0 : (uint32_t)request.level);
    return rc == H2_PAL_OK ? status_reply(d, out) : rc;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_DEVICE_SOUND_PLAY) {
    if (!d->config.audio || !d->config.vtable ||
        !d->config.vtable->resolve_sound_url)
      return H2_PAL_ERR_UNSUPPORTED;
    gizclaw_rpc_v1_ClientDeviceSoundPlayRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientDeviceSoundPlayRequest_fields,
                &request) ||
        !request.sound[0] || strlen(request.sound) >= sizeof(d->sound) ||
        (request.has_duration_ms &&
         (request.duration_ms <= 0 || request.duration_ms > 60000)))
      return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    int rc = reserve_action(d, method, out);
    if (rc == H2_PAL_OK) {
      strcpy(d->sound, request.sound);
      d->sound_ms = request.has_duration_ms ? (uint32_t)request.duration_ms : 0;
    }
    unlock(d);
    return rc;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_DEVICE_REBOOT) {
    if (!d->config.power || !d->config.power->vtable ||
        !d->config.power->vtable->reboot)
      return H2_PAL_ERR_UNSUPPORTED;
    gizclaw_rpc_v1_ClientDeviceRebootRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientDeviceRebootRequest_fields,
                &request) ||
        (request.has_delay_ms &&
         (request.delay_ms < 0 || request.delay_ms > 60000)))
      return H2_PAL_ERR_INVALID_ARG;
    lock(d);
    int rc = reserve_action(d, method, out);
    if (rc == H2_PAL_OK)
      d->delay_ms = request.has_delay_ms ? (uint32_t)request.delay_ms : 0;
    unlock(d);
    return rc;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_FIRMWARE_UPDATE) {
    const h2_gizclaw_vtable_t *v = d->config.vtable;
    if (!v || !v->ota_begin || !v->ota_write || !v->ota_finish ||
        !v->ota_abort || !v->ota_activate || !d->config.http)
      return H2_PAL_ERR_UNSUPPORTED;
    gizclaw_rpc_v1_ClientFirmwareUpdateRequest request = {0};
    if (!decode(bytes, gizclaw_rpc_v1_ClientFirmwareUpdateRequest_fields,
                &request) ||
        (request.has_sha256 && !sha_valid(request.sha256)))
      return H2_PAL_ERR_INVALID_ARG;
    int32_t channel = request.has_channel ? (int32_t)request.channel
                                          : d->config.firmware_channel;
    if (channel <= 0)
      return H2_PAL_ERR_INVALID_ARG;
    request.has_channel = true;
    request.channel = channel;
    lock(d);
    int rc = reserve_action(d, method, out);
    if (rc == H2_PAL_OK) {
      d->update = request;
      d->ota_status = (h2_gizclaw_ota_status_t){H2_GIZCLAW_OTA_RUNNING, H2_PAL_OK};
    }
    unlock(d);
    return rc;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
int h2_gizclaw_device_rpc_internal(
    void *user, h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response) {
  h2_gizclaw_device_t *d = user;
  if (!d || !response || (request.len && !request.data))
    return H2_PAL_ERR_INVALID_ARG;
  memset(response, 0, sizeof(*response));
  if (atomic_load(&d->stopping))
    return rpc_error(response, H2_GIZCLAW_RPC_ERROR_UNAVAILABLE,
                     "device stopping");
  int rc = device_rpc(d, method, request, response);
  trace(d, "rpc", method, rc);
  if (rc == H2_PAL_ERR_NOT_FOUND &&
      method != H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_FORGET &&
      d->config.rpc_provider)
    return d->config.rpc_provider(d->config.rpc_provider_user, method, request,
                                  response);
  if (rc == H2_PAL_OK)
    return rc;
  int code = H2_GIZCLAW_RPC_ERROR_INTERNAL;
  if (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_FORMAT)
    code = H2_GIZCLAW_RPC_ERROR_INVALID_ARGUMENT;
  else if (rc == H2_PAL_ERR_BUSY || rc == H2_PAL_ERR_NO_MEMORY)
    code = H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED;
  else if (rc == H2_PAL_ERR_NOT_FOUND &&
           method == H2_GIZCLAW_RPC_CLIENT_WIFI_SAVED_FORGET)
    code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND;
  else if (rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_NOT_FOUND)
    code = H2_GIZCLAW_RPC_ERROR_UNIMPLEMENTED;
  else if (rc == H2_PAL_ERR_TIMEOUT)
    code = H2_GIZCLAW_RPC_ERROR_DEADLINE_EXCEEDED;
  return rpc_error(response, code, "device operation failed");
}

static void submit_telemetry(h2_gizclaw_device_t *d,
                             const h2_gizclaw_telemetry_frame_t *frame) {
  h2_gizclaw_req_t *request = NULL;
  if (h2_gizclaw_req_create_telemetry_send(d->service, 0, frame, 1000u,
                                           &request) == H2_PAL_OK) {
    (void)h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    h2_gizclaw_req_release(request);
  }
}
static void report_player(h2_gizclaw_device_t *d) {
  gizclaw_rpc_v1_AudioPlayerStatus status;
  lock(d);
  status = d->status;
  d->dirty = false;
  unlock(d);
  h2_gizclaw_telemetry_observation_t observation = {
      .kind = H2_GIZCLAW_TELEMETRY_AUDIOPLAYER,
      .value.audioplayer =
          {
              .state = text_span(status.state),
              .repeat = text_span(status.repeat),
              .has_current_index = status.has_current_index,
              .current_index = status.current_index,
              .position_ms = status.position_ms,
              .has_duration_ms = status.has_duration_ms,
              .duration_ms = status.duration_ms,
              .playlist_length = status.playlist_length,
              .playlist_revision = status.playlist_revision,
              .has_error_code = status.has_error_code,
              .error_code = text_span(status.error_code),
              .has_error_message = status.has_error_message,
              .error_message = text_span(status.error_message),
          },
  };
  if (++d->sequence == 0)
    ++d->sequence;
  h2_gizclaw_telemetry_frame_t frame = {.sequence = d->sequence,
                                        .observations = &observation,
                                        .observation_count = 1};
  /* One bounded attempt; never stall media waiting for server persistence. */
  submit_telemetry(d, &frame);
}
static bool interrupted(h2_gizclaw_device_t *d) {
  return atomic_load(&d->stopping) ||
         atomic_load(&d->generation) != d->worker_generation;
}
static int download_cancel(void *user) { return interrupted(user); }
static uint32_t io_timeout(h2_gizclaw_device_t *d) {
  return d->config.connect_timeout_ms > 0
             ? (uint32_t)d->config.connect_timeout_ms
             : 15000u;
}
struct audio_download {
  h2_gizclaw_device_t *device;
  char url[1025];
  h2_pal_task_t *task;
  uint8_t *data;
  size_t capacity, head, count, prebuffer;
  uint64_t length;
  atomic_bool cancel;
  bool done, ready, music;
  int result;
};
static int audio_cancel(void *user) {
  audio_download_t *download = user;
  return atomic_load(&download->cancel) || interrupted(download->device);
}
static int audio_read(void *user, const h2_pal_http_request_t *request,
                      const uint8_t *chunk, size_t length, size_t total,
                      size_t remaining) {
  (void)request;
  (void)total;
  (void)remaining;
  audio_download_t *download = user;
  h2_gizclaw_device_t *d = download->device;
  size_t offset = 0;
  while (offset < length) {
    if (audio_cancel(download))
      return H2_PAL_ERR_CLOSED;
    lock(d);
    size_t count = download->capacity - download->count;
    size_t tail = (download->head + download->count) % download->capacity;
    if (count > download->capacity - tail)
      count = download->capacity - tail;
    if (count > length - offset)
      count = length - offset;
    memcpy(download->data + tail, chunk + offset, count);
    download->count += count;
    download->length += count;
    unlock(d);
    offset += count;
    if (!count)
      (void)h2_pal_time_sleep_ms(d->config.time, 5);
  }
  return H2_PAL_OK;
}
static void audio_download_worker(void *user) {
  audio_download_t *download = user;
  h2_gizclaw_device_t *d = download->device;
  uint8_t chunk[2048];
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {download->url, strlen(download->url)},
      /* Backpressure can span the whole song. The reader enforces a bounded
       * no-progress timeout and cancels this request on stop or starvation. */
      .timeout_ms = INT_MAX,
      .retry_count = 0,
      .chunk_buf = chunk,
      .chunk_buf_cap = sizeof(chunk),
      .read_cb = audio_read,
      .user = download,
      .cancel_cb = audio_cancel,
      .cancel_user = download,
      .allocator = d->config.allocator,
  };
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_request(d->config.http, &request, &response);
  if (rc == H2_PAL_OK &&
      (response.status_code < 200 || response.status_code >= 300 ||
       download->length == 0 ||
       (response.content_length >= 0 &&
        (uint64_t)response.content_length != download->length)))
    rc = H2_PAL_ERR_FORMAT;
  char message[128];
  (void)snprintf(
      message, sizeof(message), "audio-download status=%d bytes=%llu rc=%d",
      response.status_code, (unsigned long long)download->length, rc);
  (void)h2_pal_log_write(d->config.log,
                         rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN,
                         "gizclaw", message);
  h2_pal_http_response_free(d->config.http, &response);
  lock(d);
  download->result = rc;
  download->done = true;
  unlock(d);
}
static h2_pal_result_t audio_stream_read(void *user, uint8_t *out, size_t capacity,
                             size_t *out_len) {
  audio_download_t *download = user;
  h2_gizclaw_device_t *d = download->device;
  *out_len = 0;
  uint64_t progress_at = 0;
  (void)h2_pal_time_get_monotonic_ms(d->config.time, &progress_at);
  size_t previous = 0;
  for (;;) {
    if (audio_cancel(download))
      return H2_PAL_ERR_CLOSED;
    lock(d);
    if (download->done && download->result != H2_PAL_OK) {
      int rc = download->result;
      unlock(d);
      return rc;
    }
    if (!download->ready &&
        (download->count >= download->prebuffer || download->done))
      download->ready = true;
    if (download->ready && download->count) {
      size_t count = download->count;
      if (count > download->capacity - download->head)
        count = download->capacity - download->head;
      if (count > capacity)
        count = capacity;
      memcpy(out, download->data + download->head, count);
      download->head = (download->head + count) % download->capacity;
      download->count -= count;
      unlock(d);
      *out_len = count;
      return H2_PAL_OK;
    }
    if (download->done) {
      unlock(d);
      return H2_PAL_EXIT;
    }
    download->ready = false;
    bool changed = download->music && strcmp(d->status.state, "buffering") != 0;
    if (changed)
      strcpy(d->status.state, "buffering");
    size_t count = download->count;
    unlock(d);
    if (changed)
      report_player(d);
    uint64_t now = 0;
    (void)h2_pal_time_get_monotonic_ms(d->config.time, &now);
    if (count != previous) {
      previous = count;
      progress_at = now;
    }
    if (now - progress_at >= io_timeout(d)) {
      atomic_store(&download->cancel, true);
      return H2_PAL_ERR_TIMEOUT;
    }
    (void)h2_pal_time_sleep_ms(d->config.time, 5);
  }
}
static int finish_audio_download(h2_gizclaw_device_t *d) {
  audio_download_t *download = d->download;
  if (!download)
    return H2_PAL_OK;
  atomic_store(&download->cancel, true);
  if (download->task) {
    int rc = h2_pal_task_join(d->service->config.task, download->task);
    if (rc != H2_PAL_OK)
      return rc;
  }
  h2_pal_mem_free(d->config.allocator, download->data);
  h2_pal_mem_free(d->config.allocator, download);
  d->download = NULL;
  return H2_PAL_OK;
}
static int write_player_pcm(h2_gizclaw_device_t *d, h2_pal_audio_track_t *track,
                            const h2_audio_frame_t *frame) {
  uint64_t started = 0;
  int rc = h2_pal_time_get_monotonic_ms(d->config.time, &started);
  if (rc != H2_PAL_OK)
    return rc;
  for (;;) {
    if (interrupted(d))
      return H2_PAL_ERR_CLOSED;
    /* Exactly one complete PAL frame: a blocked write has consumed no frame. */
    rc = h2_pal_audio_track_write(track, frame, 100u);
    if (rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(d->config.time, &now);
    if (rc != H2_PAL_OK)
      return rc;
    if (now - started >= io_timeout(d))
      return H2_PAL_ERR_TIMEOUT;
    (void)h2_pal_time_sleep_ms(d->config.time, 5);
  }
}
static int play_url(h2_gizclaw_device_t *d, const char *url, uint32_t limit_ms,
                    bool music) {
  audio_download_t *download =
      h2_pal_mem_alloc(d->config.allocator, sizeof(*download));
  if (!download)
    return H2_PAL_ERR_NO_MEMORY;
  *download = (audio_download_t){.device = d,
                                 .music = music,
                                 .capacity = d->config.audio_buffer_bytes
                                                 ? d->config.audio_buffer_bytes
                                                 : 65536u};
  strcpy(download->url, url);
  download->prebuffer =
      d->config.audio_prebuffer_bytes
          ? d->config.audio_prebuffer_bytes
          : (download->capacity < 16384u ? download->capacity : 16384u);
  atomic_init(&download->cancel, false);
  d->download = download;
  download->data = h2_pal_mem_alloc(d->config.allocator, download->capacity);
  if (!download->data) {
    (void)finish_audio_download(d);
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_pal_task_options_t options = {
      .name = H2_GIZCLAW_AUDIO_DOWNLOAD_TASK_NAME_VALUE,
      .min_stack_size = 32768};
  int rc = h2_pal_task_start(d->service->config.task, &options,
                             audio_download_worker, download, &download->task);
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  h2_pal_audio_track_t *track = NULL;
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_ogg_opus_create_reader(
        d->config.allocator, audio_stream_read, download, &decoder);
  h2_audio_track_config_t audio = {
      .name = "gizclaw-player",
      .format = {.sample_rate_hz = 16000,
                 .frame_samples_per_channel = 320,
                 .channels = 1,
                 .sample_format = H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000,
      .buffer_frames = 8,
  };
  h2_audio_info_t info = {0};
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_get_info(d->config.audio, &info);
  if (rc == H2_PAL_OK) {
    audio.format = info.playback_format;
    if (!info.playback_supported || audio.format.sample_rate_hz != 16000 ||
        audio.format.channels != 1 ||
        audio.format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        !audio.format.frame_samples_per_channel)
      rc = H2_PAL_ERR_UNSUPPORTED;
  }
  size_t frame_bytes = (size_t)audio.format.frame_samples_per_channel * 2;
  uint8_t *output = NULL;
  if (rc == H2_PAL_OK) {
    output = h2_pal_mem_alloc(d->config.allocator, frame_bytes);
    if (!output)
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_start_speaker(d->config.audio);
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_create_track(d->config.audio, &audio, &track);
  trace(d, "player-track", 0, rc);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  uint64_t submitted_bytes = 0, reported_ms = 0;
  size_t buffered = 0;
  bool finished = false;
  while (rc == H2_PAL_OK && !interrupted(d) && !finished) {
    size_t length = 0;
    rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &length);
    if (rc == H2_PAL_EXIT) {
      rc = H2_PAL_OK;
      finished = true;
    }
    if (rc != H2_PAL_OK)
      break;
    if (limit_ms &&
        length >= (uint64_t)limit_ms * 32 - submitted_bytes - buffered) {
      length = (size_t)((uint64_t)limit_ms * 32 - submitted_bytes - buffered);
      finished = true;
    }
    for (size_t offset = 0; (offset < length || (finished && buffered)) &&
                            rc == H2_PAL_OK && !interrupted(d);) {
      size_t count = length - offset;
      if (count > frame_bytes - buffered)
        count = frame_bytes - buffered;
      memcpy(output + buffered, pcm + offset, count);
      buffered += count;
      offset += count;
      if (buffered < frame_bytes && !finished)
        continue;
      if (buffered < frame_bytes && offset < length)
        continue;
      /* PAL tracks require complete hardware frames. Pad only the final one;
       * playback progress counts source samples, excluding this silence. */
      memset(output + buffered, 0, frame_bytes - buffered);
      h2_audio_frame_t frame = {.data = output,
                                .capacity = frame_bytes,
                                .bytes = frame_bytes,
                                .sample_rate_hz = 16000,
                                .samples_per_channel =
                                    audio.format.frame_samples_per_channel,
                                .channels = 1,
                                .sample_format = H2_AUDIO_SAMPLE_S16LE};
      rc = write_player_pcm(d, track, &frame);
      if (rc != H2_PAL_OK)
        break;
      submitted_bytes += buffered;
      buffered = 0;
      /* Keep feeding the PCM queue continuously. A drain barrier on every
       * frame inserts silence in PAL mixers. During playback, conservatively
       * subtract the requested queue capacity plus one in-flight frame. */
      uint64_t pending_bytes = frame_bytes * (audio.buffer_frames + 1u);
      uint64_t position_ms = submitted_bytes > pending_bytes
                                 ? (submitted_bytes - pending_bytes) / 32u
                                 : 0u;
      lock(d);
      if (music && !interrupted(d)) {
        strcpy(d->status.state, "playing");
        d->status.position_ms = position_ms;
      }
      unlock(d);
      if (music && (position_ms - reported_ms >= 1000 || reported_ms == 0)) {
        report_player(d);
        reported_ms = position_ms;
      }
    }
  }
  h2_pal_mem_free(d->config.allocator, output);
  if (interrupted(d))
    rc = H2_PAL_ERR_CLOSED;
  if (track) {
    if (rc == H2_PAL_OK) {
      rc = h2_pal_audio_track_drain(track, 1000u);
      if (rc == H2_PAL_OK && music) {
        lock(d);
        if (!interrupted(d))
          d->status.position_ms = submitted_bytes / 32u;
        unlock(d);
      }
    }
    int closed = h2_pal_audio_track_close(track);
    if (rc == H2_PAL_OK)
      rc = closed;
  }
  h2_gizclaw_ogg_opus_destroy(decoder);
  int joined = finish_audio_download(d);
  if (rc == H2_PAL_OK)
    rc = joined;
  return rc;
}

typedef struct ota_download {
  h2_gizclaw_device_t *device;
  uint64_t expected, received;
  unsigned last_percent;
  char update_id[65];
} ota_download_t;
static void report_ota(ota_download_t *download, h2_gizclaw_ota_state_t state,
                       int result) {
  h2_gizclaw_device_t *d = download->device;
  h2_gizclaw_telemetry_observation_t observation = {
      .kind = H2_GIZCLAW_TELEMETRY_OTA,
      .value.ota = {.state = state,
                    .update_id = text_span(download->update_id),
                    .has_download_percent =
                        state == H2_GIZCLAW_OTA_STATE_DOWNLOADING,
                    .download_percent = download->expected
                                            ? 100.0 *
                                                  (double)download->received /
                                                  (double)download->expected
                                            : 0,
                    .has_error_code = result != H2_PAL_OK,
                    .has_error_message = result != H2_PAL_OK},
  };
  char error[32];
  (void)snprintf(error, sizeof(error), "pal:%d", result);
  observation.value.ota.error_code = text_span(error);
  observation.value.ota.error_message = text_span("firmware update failed");
  if (++d->sequence == 0)
    ++d->sequence;
  h2_gizclaw_telemetry_frame_t frame = {.sequence = d->sequence,
                                        .observations = &observation,
                                        .observation_count = 1};
  submit_telemetry(d, &frame);
}
static int ota_read(void *user, const h2_pal_http_request_t *request,
                    const uint8_t *chunk, size_t length, size_t total,
                    size_t remaining) {
  (void)request;
  (void)total;
  (void)remaining;
  ota_download_t *download = user;
  h2_gizclaw_device_t *d = download->device;
  if (interrupted(d))
    return H2_PAL_ERR_CLOSED;
  if ((uint64_t)length > download->expected - download->received)
    return H2_PAL_ERR_FORMAT;
  int rc = d->config.vtable->ota_write(d->config.user, chunk, length);
  if (rc != H2_PAL_OK)
    return rc;
  download->received += length;
  unsigned percent = (unsigned)(100.0 * (double)download->received /
                                (double)download->expected);
  if (percent >= download->last_percent + 5 || percent == 100) {
    download->last_percent = percent;
    report_ota(download, H2_GIZCLAW_OTA_STATE_DOWNLOADING, H2_PAL_OK);
  }
  return H2_PAL_OK;
}
static void update_firmware(h2_gizclaw_device_t *d) {
  lock(d);
  d->ota_status = (h2_gizclaw_ota_status_t){H2_GIZCLAW_OTA_RUNNING, H2_PAL_OK};
  unlock(d);
  ota_download_t download = {.device = d};
  uint8_t random[16];
  int rc = h2_pal_crypto_random(d->config.crypto, random, sizeof(random));
  if (rc != H2_PAL_OK) {
    lock(d);
    d->ota_status = (h2_gizclaw_ota_status_t){H2_GIZCLAW_OTA_FAILED, rc};
    unlock(d);
    return;
  }
  for (size_t i = 0; i < sizeof(random); ++i)
    (void)snprintf(download.update_id + i * 2, 3, "%02x", random[i]);
  report_ota(&download, H2_GIZCLAW_OTA_STATE_STARTED, H2_PAL_OK);
  h2_gizclaw_firmware_t firmware = {0};
  rc = h2_gizclaw_rpc_firmware_get(d->service, d->update.channel, io_timeout(d),
                                   &firmware);
  if (rc == H2_PAL_OK &&
      (!https_url(firmware.url) || !sha_valid(firmware.sha256) ||
       firmware.size <= 0 ||
       (d->update.has_sha256 && !sha_equal(firmware.sha256, d->update.sha256))))
    rc = H2_PAL_ERR_FORMAT;
  const h2_gizclaw_vtable_t *v = d->config.vtable;
  bool began = false;
  if (rc == H2_PAL_OK && interrupted(d))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK) {
    download.expected = (uint64_t)firmware.size;
    began = true;
    rc = v->ota_begin(d->config.user, &firmware, download.update_id);
  }
  uint8_t chunk[2048];
  if (rc == H2_PAL_OK) {
    const h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {firmware.url, strlen(firmware.url)},
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .retry_count = 0,
        .chunk_buf = chunk,
        .chunk_buf_cap = sizeof(chunk),
        .read_cb = ota_read,
        .user = &download,
        .cancel_cb = download_cancel,
        .cancel_user = d,
        .allocator = d->config.allocator,
    };
    h2_pal_http_response_t response = {0};
    rc = h2_pal_http_request(d->config.http, &request, &response);
    if (rc == H2_PAL_OK &&
        (response.status_code < 200 || response.status_code >= 300 ||
         download.received != download.expected ||
         (response.content_length >= 0 &&
          (uint64_t)response.content_length != download.expected)))
      rc = H2_PAL_ERR_FORMAT;
    h2_pal_http_response_free(d->config.http, &response);
  }
  if (rc == H2_PAL_OK && interrupted(d))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK)
    rc = v->ota_finish(d->config.user);
  if (rc == H2_PAL_OK)
    rc = v->ota_activate(d->config.user);
  if (rc != H2_PAL_OK) {
    if (began)
      v->ota_abort(d->config.user);
    report_ota(&download, H2_GIZCLAW_OTA_STATE_FAILED, rc);
  }
  lock(d);
  d->ota_status = (h2_gizclaw_ota_status_t){
      rc == H2_PAL_OK ? H2_GIZCLAW_OTA_STAGED : H2_GIZCLAW_OTA_FAILED, rc};
  unlock(d);
}

static void device_worker(void *user) {
  h2_gizclaw_device_t *d = user;
  while (!atomic_load(&d->stopping)) {
    if (finish_audio_download(d) != H2_PAL_OK) {
      (void)h2_pal_time_sleep_ms(d->config.time, 20);
      continue;
    }

    char url[1025] = {0};
    lock(d);
    bool playing = d->playing;
    bool dirty = d->dirty && d->pending == 0;
    int pending = d->pending_ready ? d->pending : 0;
    if (playing)
      strcpy(url, d->playlist->items[d->status.current_index].url);
    d->worker_generation = atomic_load(&d->generation);
    unlock(d);
    if (dirty && d->config.audio != NULL)
      report_player(d);
    if (pending) {
      trace(d, "action", pending, H2_PAL_OK);
      if (pending == H2_GIZCLAW_RPC_CLIENT_FIRMWARE_UPDATE)
        update_firmware(d);
      else if (pending == H2_GIZCLAW_RPC_CLIENT_DEVICE_SOUND_PLAY) {
        char sound_url[1025] = {0};
        int result = d->config.vtable->resolve_sound_url(
            d->config.user, d->sound, sound_url, sizeof(sound_url));
        if (result == H2_PAL_OK && sound_url[1024] == 0 && https_url(sound_url))
          (void)play_url(d, sound_url, d->sound_ms, false);
      } else if (pending == H2_GIZCLAW_RPC_CLIENT_WIFI_CONNECT) {
        int rc = h2_pal_wifi_sta_connect(d->config.wifi, &d->wifi_config,
                                         io_timeout(d));
        trace(d, "wifi_connect", pending, rc);
      } else if (pending == H2_GIZCLAW_RPC_CLIENT_DEVICE_REBOOT) {
        uint32_t remaining = d->delay_ms;
        while (remaining && !atomic_load(&d->stopping)) {
          uint32_t step = remaining > 20 ? 20 : remaining;
          (void)h2_pal_time_sleep_ms(d->config.time, step);
          remaining -= step;
        }
        if (!atomic_load(&d->stopping))
          (void)h2_pal_power_reboot(d->config.power, 0);
      }
      lock(d);
      d->pending = 0;
      d->pending_ready = false;
      memset(&d->wifi_config, 0, sizeof(d->wifi_config));
      unlock(d);
    } else if (playing) {
      int rc = play_url(d, url, 0, true);
      lock(d);
      if (!interrupted(d)) {
        if (rc == H2_PAL_OK) {
          d->status.has_duration_ms = true;
          d->status.duration_ms = d->status.position_ms;
          bool one = !strcmp(d->status.repeat, "one");
          bool next = d->status.current_index + 1 < d->playlist->items_count;
          bool all = !strcmp(d->status.repeat, "all");
          if (one || next || all) {
            if (!one)
              d->status.current_index = next ? d->status.current_index + 1 : 0;
            d->status.position_ms = 0;
            d->status.has_duration_ms = false;
            strcpy(d->status.state, "buffering");
          } else {
            d->playing = false;
            strcpy(d->status.state, "ended");
          }
        } else {
          d->playing = false;
          strcpy(d->status.state, "error");
          d->status.has_error_code = d->status.has_error_message = true;
          (void)snprintf(d->status.error_code, sizeof(d->status.error_code),
                         "pal:%d", rc);
          strcpy(d->status.error_message, "audio playback failed");
        }
        changed(d);
      }
      unlock(d);
    } else if (h2_pal_time_sleep_ms(d->config.time, 20u) != H2_PAL_OK)
      break;
  }
}

h2_pal_result_t h2_gizclaw_device_init_internal(h2_gizclaw_service_t *service) {
  const h2_gizclaw_config_t *config = &service->client_config;
  if (!config->audio && !config->wifi && !config->wifi_settings &&
      !config->power && !config->vtable && !config->manufacturer &&
      !config->model && !config->serial && !config->hardware_revision)
    return H2_PAL_OK;
  size_t audio_capacity =
      config->audio_buffer_bytes ? config->audio_buffer_bytes : 65536u;
  if (config->audio && config->audio_prebuffer_bytes > audio_capacity)
    return H2_PAL_ERR_INVALID_ARG;
  if (!config->time->vtable->sleep_ms)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = h2_pal_mem_alloc(config->allocator, sizeof(*d));
  if (!d)
    return H2_PAL_ERR_NO_MEMORY;
  memset(d, 0, sizeof(*d));
  d->config = *config;
  d->service = service;
  atomic_init(&d->stopping, false);
  atomic_init(&d->generation, 0);
  strcpy(d->status.state, "stopped");
  strcpy(d->status.repeat, "off");
  const h2_pal_mutex_config_t mutex = {.name = "gizclaw-device",
                                       .allocator = config->allocator};
  int rc = h2_pal_mutex_create(service->config.sync, &mutex, &d->mutex);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->allocator, d);
    return rc;
  }
  if (config->audio) {
    d->playlist = h2_pal_mem_alloc(config->allocator, sizeof(*d->playlist));
    d->incoming = h2_pal_mem_alloc(config->allocator, sizeof(*d->incoming));
    if (!d->playlist || !d->incoming) {
      h2_gizclaw_device_destroy_internal(d);
      return H2_PAL_ERR_NO_MEMORY;
    }
    memset(d->playlist, 0, sizeof(*d->playlist));
    memset(d->incoming, 0, sizeof(*d->incoming));
  }
  service->device = d;
  service->client_config.rpc_provider = h2_gizclaw_device_rpc_internal;
  service->client_config.rpc_provider_user = d;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_device_start_internal(h2_gizclaw_device_t *d) {
  if (!d)
    return H2_PAL_OK;
  const h2_pal_task_options_t options = {
      .name = H2_GIZCLAW_DEVICE_TASK_NAME_VALUE, .min_stack_size = 32768};
  return h2_pal_task_start(d->service->config.task, &options, device_worker, d,
                           &d->task);
}
void h2_gizclaw_device_cancel_internal(h2_gizclaw_device_t *d) {
  if (d)
    atomic_store(&d->stopping, true);
}
h2_pal_result_t h2_gizclaw_device_stop_internal(h2_gizclaw_device_t *d) {
  if (!d)
    return H2_PAL_OK;
  h2_gizclaw_device_cancel_internal(d);
  if (!d->task)
    return finish_audio_download(d);
  int rc = h2_pal_task_join(d->service->config.task, d->task);
  if (rc == H2_PAL_OK) {
    d->task = NULL;
    rc = finish_audio_download(d);
  }
  return rc;
}
void h2_gizclaw_device_destroy_internal(h2_gizclaw_device_t *d) {
  if (!d || d->task || d->download)
    return;
  (void)h2_pal_mutex_destroy(d->service->config.sync, d->mutex);
  h2_pal_mem_free(d->config.allocator, d->response);
  h2_pal_mem_free(d->config.allocator, d->incoming);
  h2_pal_mem_free(d->config.allocator, d->playlist);
  h2_pal_mem_free(d->config.allocator, d);
}

/* Local controls only publish commands/snapshots under the same mutex as RPC.
 * The caller never performs HTTP, firmware queries or PCM writes. */
h2_pal_result_t h2_gizclaw_player_play(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t url) {
  if (!service || !url.data || !url.len || url.len > 1024 ||
      memchr(url.data, 0, url.len))
    return H2_PAL_ERR_INVALID_ARG;
  char copy[1025];
  memcpy(copy, url.data, url.len);
  copy[url.len] = 0;
  if (!https_url(copy))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = service->device;
  if (!d || !d->playlist)
    return H2_PAL_ERR_UNSUPPORTED;
  lock(d);
  int rc = H2_PAL_OK;
  if (!d->task || atomic_load(&d->stopping))
    rc = H2_PAL_ERR_CLOSED;
  else if (d->pending)
    rc = H2_PAL_ERR_BUSY;
  else {
    cancel_play_locked(d);
    memset(d->playlist->items, 0, sizeof(d->playlist->items));
    strcpy(d->playlist->items[0].url, copy);
    d->playlist->items_count = 1;
    ++d->playlist->playlist_revision;
    d->status.playlist_revision = d->playlist->playlist_revision;
    d->status.playlist_length = 1;
    d->status.has_current_index = true;
    d->status.current_index = 0;
    d->status.position_ms = 0;
    d->status.has_duration_ms = false;
    d->status.has_error_code = d->status.has_error_message = false;
    strcpy(d->status.repeat, "off");
    strcpy(d->status.state, "buffering");
    d->playing = true;
    changed(d);
  }
  unlock(d);
  return rc;
}
h2_pal_result_t h2_gizclaw_player_stop(h2_gizclaw_service_t *service) {
  if (!service)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = service->device;
  if (!d || !d->playlist)
    return H2_PAL_ERR_UNSUPPORTED;
  lock(d);
  int rc = H2_PAL_OK;
  if (atomic_load(&d->stopping))
    rc = H2_PAL_ERR_CLOSED;
  else if (d->pending &&
           d->pending != H2_GIZCLAW_RPC_CLIENT_DEVICE_AUDIOPLAYER_PLAY)
    rc = H2_PAL_ERR_BUSY;
  else
    cancel_play_locked(d);
  unlock(d);
  return rc;
}
h2_pal_result_t h2_gizclaw_player_get_status(h2_gizclaw_service_t *service,
                                             h2_gizclaw_player_status_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!service)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = service->device;
  if (!d || !d->playlist)
    return H2_PAL_ERR_UNSUPPORTED;
  lock(d);
  strcpy(out->state, d->status.state);
  out->position_ms = d->status.position_ms;
  out->has_duration_ms = d->status.has_duration_ms;
  out->duration_ms = d->status.duration_ms;
  if (d->status.has_error_code)
    strcpy(out->error_code, d->status.error_code);
  if (d->status.has_error_message)
    strcpy(out->error_message, d->status.error_message);
  unlock(d);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_ota_start(h2_gizclaw_service_t *service,
                                     int32_t channel,
                                     h2_gizclaw_str_t expected_sha256) {
  if (!service || (expected_sha256.len && !expected_sha256.data))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = service->device;
  if (!d)
    return H2_PAL_ERR_UNSUPPORTED;
  const h2_gizclaw_vtable_t *v = d->config.vtable;
  if (!v || !v->ota_begin || !v->ota_write || !v->ota_finish || !v->ota_abort ||
      !v->ota_activate || !d->config.http)
    return H2_PAL_ERR_UNSUPPORTED;
  gizclaw_rpc_v1_ClientFirmwareUpdateRequest update = {.has_channel = true};
  update.channel = channel ? channel : d->config.firmware_channel;
  if ((int32_t)update.channel <= 0)
    return H2_PAL_ERR_INVALID_ARG;
  if (expected_sha256.len) {
    if (expected_sha256.len != 64)
      return H2_PAL_ERR_INVALID_ARG;
    memcpy(update.sha256, expected_sha256.data, 64);
    if (!sha_valid(update.sha256))
      return H2_PAL_ERR_INVALID_ARG;
    update.has_sha256 = true;
  }
  lock(d);
  int rc = H2_PAL_OK;
  if (!d->task || atomic_load(&d->stopping))
    rc = H2_PAL_ERR_CLOSED;
  else if (d->pending)
    rc = H2_PAL_ERR_BUSY;
  else {
    d->update = update;
    d->ota_status = (h2_gizclaw_ota_status_t){H2_GIZCLAW_OTA_RUNNING, H2_PAL_OK};
    cancel_play_locked(d);
    d->pending = H2_GIZCLAW_RPC_CLIENT_FIRMWARE_UPDATE;
    d->pending_ready = true;
  }
  unlock(d);
  return rc;
}

h2_pal_result_t h2_gizclaw_ota_get_status(h2_gizclaw_service_t *service,
                                         h2_gizclaw_ota_status_t *out_status) {
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_status, 0, sizeof(*out_status));
  if (service == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_device_t *d = service->device;
  if (d == NULL) return H2_PAL_ERR_UNSUPPORTED;
  lock(d);
  *out_status = d->ota_status;
  unlock(d);
  return H2_PAL_OK;
}
