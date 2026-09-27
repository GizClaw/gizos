#include "h2_gizclaw_mhs_internal.h"

#include "payload/mhs.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool key_valid(const char *key) {
  if (!key || key[0] < 'a' || key[0] > 'z')
    return false;
  bool separator = false;
  for (size_t i = 0; i <= H2_GIZCLAW_MHS_KEY_MAX; ++i) {
    char c = key[i];
    if (!c)
      return !separator;
    if (i == H2_GIZCLAW_MHS_KEY_MAX)
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

static bool utf8_valid(const uint8_t *s, size_t n) {
  for (size_t i = 0; i < n;) {
    uint32_t c = s[i++], min = 0;
    size_t rest = 0;
    if (!c)
      return false;
    if (c < 0x80)
      continue;
    if (c >= 0xc2 && c <= 0xdf) {
      c &= 0x1f;
      rest = 1;
      min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      c &= 0x0f;
      rest = 2;
      min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      c &= 7;
      rest = 3;
      min = 0x10000;
    } else
      return false;
    if (n - i < rest)
      return false;
    while (rest--) {
      if ((s[i] & 0xc0) != 0x80)
        return false;
      c = (c << 6) | (s[i++] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return false;
  }
  return true;
}

static bool value_valid(const h2_gizclaw_mhs_value_t *v,
                        h2_gizclaw_mhs_kind_t kind) {
  if (v->kind != kind)
    return false;
  switch (kind) {
  case H2_GIZCLAW_MHS_BOOL:
    return true;
  case H2_GIZCLAW_MHS_INT:
    return v->value.i >= -INT64_C(9007199254740991) &&
           v->value.i <= INT64_C(9007199254740991);
  case H2_GIZCLAW_MHS_DOUBLE:
    return isfinite(v->value.d);
  case H2_GIZCLAW_MHS_STRING: {
    const char *end = memchr(v->value.s, 0, sizeof(v->value.s));
    return end &&
           utf8_valid((const uint8_t *)v->value.s, (size_t)(end - v->value.s));
  }
  default:
    return false;
  }
}

int h2_gizclaw_mhs_validate_internal(const h2_gizclaw_mhs_state_t *states,
                                     size_t count) {
  if ((count && !states) || count > SIZE_MAX / sizeof(*states))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < count; ++i) {
    const h2_gizclaw_mhs_state_t *s = &states[i];
    if (!key_valid(s->device_id) || !key_valid(s->state) || !s->read ||
        s->kind < H2_GIZCLAW_MHS_BOOL || s->kind > H2_GIZCLAW_MHS_STRING)
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(s->device_id, states[j].device_id) &&
          !strcmp(s->state, states[j].state))
        return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

static int speaker_snapshot(h2_gizclaw_mhs_builtin_t *b,
                            h2_runtime_system_audio_state_t *out) {
  if (b->runtime && b->audio == b->runtime->audio)
    return h2_runtime_system_state_audio(b->runtime, out);
  int rc =
      h2_pal_audio_get_speaker_volume_percent(b->audio, &out->volume_percent);
  out->muted = out->volume_percent == 0;
  return rc;
}
static h2_pal_result_t speaker_read(void *user, h2_gizclaw_mhs_value_t *out) {
  h2_runtime_system_audio_state_t snapshot = {0};
  int rc = speaker_snapshot(user, &snapshot);
  if (rc == H2_PAL_OK) {
    if (out->kind == H2_GIZCLAW_MHS_INT)
      out->value.i = snapshot.volume_percent;
    else
      out->value.b = snapshot.muted;
  }
  return rc;
}
static h2_pal_result_t speaker_check(void *user,
                                     const h2_gizclaw_mhs_value_t *v) {
  (void)user;
  return v->kind == H2_GIZCLAW_MHS_INT && (v->value.i < 0 || v->value.i > 100)
             ? H2_PAL_ERR_INVALID_ARG
             : H2_PAL_OK;
}
static int speaker_apply(h2_gizclaw_mhs_builtin_t *b, uint32_t volume,
                         bool muted) {
  return b->runtime && b->audio == b->runtime->audio
             ? h2_runtime_audio_set_volume(b->runtime, volume, muted)
             : h2_pal_audio_set_speaker_volume_percent(b->audio,
                                                       muted ? 0 : volume);
}
static h2_pal_result_t speaker_write(void *user,
                                     const h2_gizclaw_mhs_value_t *v,
                                     h2_gizclaw_mhs_value_t *out) {
  h2_runtime_system_audio_state_t snapshot = {0};
  int rc = speaker_snapshot(user, &snapshot);
  if (rc != H2_PAL_OK)
    return rc;
  if (v->kind == H2_GIZCLAW_MHS_INT)
    snapshot.volume_percent = (uint32_t)v->value.i;
  else
    snapshot.muted = v->value.b;
  rc = speaker_apply(user, snapshot.volume_percent, snapshot.muted);
  out->kind = v->kind;
  return rc == H2_PAL_OK ? speaker_read(user, out) : rc;
}

/* Separate callbacks keep each Wi-Fi key explicit without a second registry. */
static int wifi_read(h2_gizclaw_mhs_builtin_t *b, int field,
                     h2_gizclaw_mhs_value_t *out) {
  h2_pal_wifi_sta_status_t status = {0};
  int rc = h2_pal_wifi_sta_get_status(b->wifi, &status);
  if (rc != H2_PAL_OK)
    return rc;
  if (field == 1 &&
      (status.ssid_len > 32 ||
       !utf8_valid((const uint8_t *)status.ssid, status.ssid_len)))
    return H2_PAL_ERR_IO;
  if (field == 0) {
    out->value.b = status.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
                   status.state == H2_PAL_WIFI_STA_STATE_GOT_IP;
  } else if (field == 1) {
    memcpy(out->value.s, status.ssid, status.ssid_len);
    out->value.s[status.ssid_len] = 0;
  } else if (field == 2) {
    out->value.i = status.rssi;
  } else if (field == 3 && status.ip_valid) {
    uint8_t ip[4];
    h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
    (void)snprintf(out->value.s, sizeof(out->value.s), "%u.%u.%u.%u", ip[0],
                   ip[1], ip[2], ip[3]);
  } else if (field == 4 && status.bssid_set) {
    const uint8_t *m = status.bssid;
    (void)snprintf(out->value.s, sizeof(out->value.s),
                   "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3],
                   m[4], m[5]);
  }
  return H2_PAL_OK;
}
static h2_pal_result_t wifi_connected(void *u, h2_gizclaw_mhs_value_t *v) {
  return wifi_read(u, 0, v);
}
static h2_pal_result_t wifi_ssid(void *u, h2_gizclaw_mhs_value_t *v) {
  return wifi_read(u, 1, v);
}
static h2_pal_result_t wifi_rssi(void *u, h2_gizclaw_mhs_value_t *v) {
  return wifi_read(u, 2, v);
}
static h2_pal_result_t wifi_ip(void *u, h2_gizclaw_mhs_value_t *v) {
  return wifi_read(u, 3, v);
}
static h2_pal_result_t wifi_bssid(void *u, h2_gizclaw_mhs_value_t *v) {
  return wifi_read(u, 4, v);
}

size_t h2_gizclaw_mhs_builtins_internal(h2_gizclaw_mhs_builtin_t *b,
                                        h2_gizclaw_mhs_state_t *states) {
  size_t n = 0;
  if (b->audio && b->audio->vtable &&
      b->audio->vtable->get_speaker_volume_percent) {
    const bool writable = b->audio->vtable->set_speaker_volume_percent != NULL;
    states[n++] = (h2_gizclaw_mhs_state_t){"speaker.main",
                                           "volume",
                                           H2_GIZCLAW_MHS_INT,
                                           speaker_read,
                                           speaker_check,
                                           writable ? speaker_write : NULL,
                                           b};
    states[n++] = (h2_gizclaw_mhs_state_t){"speaker.main",
                                           "muted",
                                           H2_GIZCLAW_MHS_BOOL,
                                           speaker_read,
                                           speaker_check,
                                           writable ? speaker_write : NULL,
                                           b};
  }
  if (b->wifi && b->wifi->vtable && b->wifi->vtable->get_status) {
    states[n++] = (h2_gizclaw_mhs_state_t){"wifi.main",
                                           "connected",
                                           H2_GIZCLAW_MHS_BOOL,
                                           wifi_connected,
                                           NULL,
                                           NULL,
                                           b};
    states[n++] = (h2_gizclaw_mhs_state_t){
        "wifi.main", "ssid", H2_GIZCLAW_MHS_STRING, wifi_ssid, NULL, NULL, b};
    states[n++] = (h2_gizclaw_mhs_state_t){
        "wifi.main", "rssi-dbm", H2_GIZCLAW_MHS_INT, wifi_rssi, NULL, NULL, b};
    states[n++] = (h2_gizclaw_mhs_state_t){
        "wifi.main", "ip", H2_GIZCLAW_MHS_STRING, wifi_ip, NULL, NULL, b};
    states[n++] = (h2_gizclaw_mhs_state_t){
        "wifi.main", "bssid", H2_GIZCLAW_MHS_STRING, wifi_bssid, NULL, NULL, b};
  }
  return n;
}

/* nanopb's static strings are C strings after decode. Validate their original
 * spans too, so an embedded/trailing wire NUL cannot truncate a key or value.
 * The levels are request, state, value; unknown fields remain
 * forward-compatible. */
static bool wire_text_valid(pb_istream_t *in, unsigned level, bool write) {
  unsigned seen = 0;
  while (in->bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof = false;
    if (!pb_decode_tag(in, &wire, &tag, &eof))
      return false;
    bool nested = (level == 0 && tag == 1) || (level == 1 && write && tag == 3);
    bool text =
        (level == 1 && (tag == 1 || tag == 2)) || (level == 2 && tag == 4);
    if (level == 2 && tag >= 1 && tag <= 4 && ++seen > 1)
      return false;
    if (nested || text) {
      pb_istream_t sub;
      if (wire != PB_WT_STRING || !pb_make_string_substream(in, &sub))
        return false;
      if (nested) {
        if (!wire_text_valid(&sub, level + 1, write))
          return false;
      } else {
        uint8_t bytes[H2_GIZCLAW_MHS_STRING_MAX];
        size_t n = sub.bytes_left;
        size_t max = level == 1 ? H2_GIZCLAW_MHS_KEY_MAX : sizeof(bytes);
        if (n > max || !pb_read(&sub, bytes, n) || !utf8_valid(bytes, n))
          return false;
      }
      if (!pb_close_string_substream(in, &sub))
        return false;
    } else if (!pb_skip_field(in, wire))
      return false;
  }
  return true;
}

static void from_wire(const gizclaw_rpc_v1_MhsValue *wire,
                      h2_gizclaw_mhs_value_t *v) {
  memset(v, 0, sizeof(*v));
  switch (wire->which_value) {
  case gizclaw_rpc_v1_MhsValue_bool_value_tag:
    v->kind = H2_GIZCLAW_MHS_BOOL;
    v->value.b = wire->value.bool_value;
    break;
  case gizclaw_rpc_v1_MhsValue_int_value_tag:
    v->kind = H2_GIZCLAW_MHS_INT;
    v->value.i = wire->value.int_value;
    break;
  case gizclaw_rpc_v1_MhsValue_double_value_tag:
    v->kind = H2_GIZCLAW_MHS_DOUBLE;
    v->value.d = wire->value.double_value;
    break;
  case gizclaw_rpc_v1_MhsValue_string_value_tag:
    v->kind = H2_GIZCLAW_MHS_STRING;
    memcpy(v->value.s, wire->value.string_value, sizeof(v->value.s));
    break;
  default:
    break;
  }
}
static void to_wire(const h2_gizclaw_mhs_value_t *v,
                    gizclaw_rpc_v1_MhsValue *wire) {
  memset(wire, 0, sizeof(*wire));
  switch (v->kind) {
  case H2_GIZCLAW_MHS_BOOL:
    wire->which_value = gizclaw_rpc_v1_MhsValue_bool_value_tag;
    wire->value.bool_value = v->value.b;
    break;
  case H2_GIZCLAW_MHS_INT:
    wire->which_value = gizclaw_rpc_v1_MhsValue_int_value_tag;
    wire->value.int_value = v->value.i;
    break;
  case H2_GIZCLAW_MHS_DOUBLE:
    wire->which_value = gizclaw_rpc_v1_MhsValue_double_value_tag;
    wire->value.double_value = v->value.d;
    break;
  case H2_GIZCLAW_MHS_STRING:
    wire->which_value = gizclaw_rpc_v1_MhsValue_string_value_tag;
    memcpy(wire->value.string_value, v->value.s, sizeof(v->value.s));
    break;
  }
}

typedef struct mhs_call {
  union {
    gizclaw_rpc_v1_ClientMhsV0ReadRequest read;
    gizclaw_rpc_v1_ClientMhsV0WriteRequest write;
  } request;
  gizclaw_rpc_v1_ClientMhsV0WriteResponse reply;
} mhs_call_t;

static int execute(const h2_gizclaw_mhs_state_t *states, size_t count,
                   bool write, mhs_call_t *call) {
  const h2_gizclaw_mhs_state_t *bound[32] = {0};
  size_t n = write ? call->request.write.states_count
                   : call->request.read.states_count;
  if (!n)
    return H2_PAL_ERR_INVALID_ARG;
  call->reply.states_count = (pb_size_t)n;
  for (size_t i = 0; i < n; ++i) {
    const char *device = write ? call->request.write.states[i].device_id
                               : call->request.read.states[i].device_id;
    const char *key = write ? call->request.write.states[i].state
                            : call->request.read.states[i].state;
    if (!key_valid(device) || !key_valid(key))
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(device, call->reply.states[j].device_id) &&
          !strcmp(key, call->reply.states[j].state))
        return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0; j < count; ++j)
      if (!strcmp(device, states[j].device_id) &&
          !strcmp(key, states[j].state)) {
        bound[i] = &states[j];
        break;
      }
    if (!bound[i])
      return H2_PAL_ERR_NOT_FOUND;
    strcpy(call->reply.states[i].device_id, device);
    strcpy(call->reply.states[i].state, key);
    call->reply.states[i].has_value = true;
    if (write) {
      h2_gizclaw_mhs_value_t value;
      from_wire(&call->request.write.states[i].value, &value);
      if (!call->request.write.states[i].has_value || !bound[i]->write ||
          !value_valid(&value, bound[i]->kind))
        return H2_PAL_ERR_INVALID_ARG;
    }
  }
  /* Check the complete batch before invoking any driver write. */
  if (write)
    for (size_t i = 0; i < n; ++i) {
      h2_gizclaw_mhs_value_t value;
      from_wire(&call->request.write.states[i].value, &value);
      if (bound[i]->check) {
        int rc = bound[i]->check(bound[i]->user, &value);
        if (rc != H2_PAL_OK)
          return rc;
      }
    }
  bool done[32] = {0};
  for (size_t i = 0; i < n; ++i) {
    if (done[i])
      continue;
    const h2_gizclaw_mhs_state_t *s = bound[i];
    h2_gizclaw_mhs_value_t value = {.kind = s->kind},
                           applied = {.kind = s->kind};
    int rc;
    if (write && s->write == speaker_write) {
      h2_runtime_system_audio_state_t snapshot = {0};
      rc = speaker_snapshot(s->user, &snapshot);
      if (rc != H2_PAL_OK)
        return rc;
      for (size_t j = i; j < n; ++j)
        if (bound[j]->write == speaker_write && bound[j]->user == s->user) {
          from_wire(&call->request.write.states[j].value, &value);
          if (value.kind == H2_GIZCLAW_MHS_INT)
            snapshot.volume_percent = (uint32_t)value.value.i;
          else
            snapshot.muted = value.value.b;
        }
      rc = speaker_apply(s->user, snapshot.volume_percent, snapshot.muted);
      if (rc != H2_PAL_OK)
        return rc;
      for (size_t j = i; j < n; ++j)
        if (bound[j]->write == speaker_write && bound[j]->user == s->user) {
          applied = (h2_gizclaw_mhs_value_t){.kind = bound[j]->kind};
          rc = speaker_read(s->user, &applied);
          if (rc != H2_PAL_OK)
            return rc;
          if (!value_valid(&applied, bound[j]->kind))
            return H2_PAL_ERR_IO;
          to_wire(&applied, &call->reply.states[j].value);
          done[j] = true;
        }
      continue;
    }
    if (write) {
      from_wire(&call->request.write.states[i].value, &value);
      rc = s->write(s->user, &value, &applied);
    } else
      rc = s->read(s->user, &applied);
    if (rc != H2_PAL_OK)
      return rc;
    if (!value_valid(&applied, s->kind))
      return H2_PAL_ERR_IO;
    to_wire(&applied, &call->reply.states[i].value);
  }
  return H2_PAL_OK;
}

int h2_gizclaw_mhs_request_internal(
    const h2_gizclaw_mhs_state_t *states, size_t count, bool write,
    const h2_pal_mem_api_t *allocator, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response, uint8_t **storage) {
  if (!response || !storage || !allocator || (request.len && !request.data))
    return H2_PAL_ERR_INVALID_ARG;
  *storage = NULL;
  memset(response, 0, sizeof(*response));
  pb_istream_t input = pb_istream_from_buffer(request.data, request.len);
  if (!wire_text_valid(&input, 0, write))
    return H2_PAL_ERR_INVALID_ARG;
  mhs_call_t *call = h2_pal_mem_alloc(allocator, sizeof(*call));
  if (!call)
    return H2_PAL_ERR_NO_MEMORY;
  memset(call, 0, sizeof(*call));
  input = pb_istream_from_buffer(request.data, request.len);
  const pb_msgdesc_t *fields =
      write ? gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields
            : gizclaw_rpc_v1_ClientMhsV0ReadRequest_fields;
  int rc = pb_decode(&input, fields, &call->request)
               ? execute(states, count, write, call)
               : H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK) {
    size_t size = 0;
    fields = write ? gizclaw_rpc_v1_ClientMhsV0WriteResponse_fields
                   : gizclaw_rpc_v1_ClientMhsV0ReadResponse_fields;
    if (!pb_get_encoded_size(&size, fields, &call->reply))
      rc = H2_PAL_ERR_IO;
    else if (!(*storage = h2_pal_mem_alloc(allocator, size)))
      rc = H2_PAL_ERR_NO_MEMORY;
    else {
      pb_ostream_t output = pb_ostream_from_buffer(*storage, size);
      if (!pb_encode(&output, fields, &call->reply))
        rc = H2_PAL_ERR_IO;
      else
        response->payload =
            (h2_gizclaw_rpc_bytes_t){*storage, output.bytes_written};
    }
  }
  h2_pal_mem_free(allocator, call);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, *storage);
    *storage = NULL;
  }
  return rc;
}
