#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_profile_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include <string.h>

static int read_varint(const uint8_t *data, size_t len, size_t *offset,
                       uint64_t *out) {
  uint64_t value = 0u;
  for (unsigned int shift = 0u; shift < 64u; shift += 7u) {
    if (*offset >= len)
      return H2_PAL_ERR_FORMAT;
    const uint8_t byte = data[(*offset)++];
    if (shift == 63u && (byte & 0xfeu) != 0u)
      return H2_PAL_ERR_FORMAT;
    value |= (uint64_t)(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0u) {
      *out = value;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_FORMAT;
}

static size_t varint_size(size_t value) {
  size_t count = 1u;
  while (value >= 0x80u) {
    value >>= 7u;
    ++count;
  }
  return count;
}

static void write_varint(uint8_t *out, size_t *offset, size_t value) {
  while (value >= 0x80u) {
    out[(*offset)++] = (uint8_t)((value & 0x7fu) | 0x80u);
    value >>= 7u;
  }
  out[(*offset)++] = (uint8_t)value;
}

static int read_bytes(const uint8_t *data, size_t len, size_t *offset,
                      const uint8_t **out_data, size_t *out_len) {
  uint64_t length = 0u;
  int rc = read_varint(data, len, offset, &length);
  if (rc != H2_PAL_OK || length > SIZE_MAX || (size_t)length > len - *offset)
    return H2_PAL_ERR_FORMAT;
  *out_data = data + *offset;
  *out_len = (size_t)length;
  *offset += (size_t)length;
  return H2_PAL_OK;
}

static int skip_field(const uint8_t *data, size_t len, size_t *offset,
                      uint8_t wire_type) {
  uint64_t ignored = 0u;
  const uint8_t *bytes = NULL;
  size_t bytes_len = 0u;
  switch (wire_type) {
  case 0u:
    return read_varint(data, len, offset, &ignored);
  case 1u:
    if (len - *offset < 8u)
      return H2_PAL_ERR_FORMAT;
    *offset += 8u;
    return H2_PAL_OK;
  case 2u:
    return read_bytes(data, len, offset, &bytes, &bytes_len);
  case 5u:
    if (len - *offset < 4u)
      return H2_PAL_ERR_FORMAT;
    *offset += 4u;
    return H2_PAL_OK;
  default:
    return H2_PAL_ERR_FORMAT;
  }
}

static int copy_profile_text(char *out, size_t capacity, const uint8_t *data,
                             size_t len) {
  if (len >= capacity)
    return H2_PAL_ERR_FORMAT;
  if (len > 0u)
    memcpy(out, data, len);
  out[len] = '\0';
  return H2_PAL_OK;
}

static int decode_device_info(const uint8_t *data, size_t len,
                              h2_gizclaw_profile_t *out_profile) {
  size_t offset = 0u;
  while (offset < len) {
    uint64_t key = 0u;
    int rc = read_varint(data, len, &offset, &key);
    if (rc != H2_PAL_OK || key == 0u)
      return H2_PAL_ERR_FORMAT;
    const uint32_t field = (uint32_t)(key >> 3u);
    const uint8_t wire = (uint8_t)(key & 7u);
    if ((field == 2u || field == 4u) && wire == 2u) {
      const uint8_t *text = NULL;
      size_t text_len = 0u;
      rc = read_bytes(data, len, &offset, &text, &text_len);
      if (rc != H2_PAL_OK)
        return rc;
      if (field == 2u) {
        rc = copy_profile_text(out_profile->name, sizeof(out_profile->name),
                               text, text_len);
        out_profile->has_name = rc == H2_PAL_OK;
      } else {
        rc = copy_profile_text(out_profile->emoji, sizeof(out_profile->emoji),
                               text, text_len);
        out_profile->has_emoji = rc == H2_PAL_OK;
      }
      if (rc != H2_PAL_OK)
        return rc;
    } else {
      rc = skip_field(data, len, &offset, wire);
      if (rc != H2_PAL_OK)
        return rc;
    }
  }
  return H2_PAL_OK;
}

static int encode_profile_text_request(h2_gizclaw_str_t text, size_t max_bytes,
                                       uint8_t field_key, uint8_t *out,
                                       size_t capacity, size_t *out_len) {
  if (out == NULL || out_len == NULL || text.data == NULL || text.len == 0u ||
      text.len > max_bytes) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t profile_len = 1u + varint_size(text.len) + text.len;
  const size_t total_len = 1u + varint_size(profile_len) + profile_len;
  if (capacity < total_len)
    return H2_PAL_ERR_NO_MEMORY;
  size_t offset = 0u;
  out[offset++] = 0x0au;
  write_varint(out, &offset, profile_len);
  out[offset++] = field_key;
  write_varint(out, &offset, text.len);
  memcpy(out + offset, text.data, text.len);
  offset += text.len;
  *out_len = offset;
  return H2_PAL_OK;
}

int h2_gizclaw_profile_encode_name_request(h2_gizclaw_str_t name, uint8_t *out,
                                           size_t capacity, size_t *out_len) {
  return encode_profile_text_request(name, H2_GIZCLAW_PROFILE_NAME_MAX_BYTES,
                                     0x0au, out, capacity, out_len);
}

int h2_gizclaw_profile_encode_emoji_request(h2_gizclaw_str_t emoji,
                                            uint8_t *out, size_t capacity,
                                            size_t *out_len) {
  return encode_profile_text_request(emoji, H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES,
                                     0x12u, out, capacity, out_len);
}

int h2_gizclaw_profile_decode_info_response(const uint8_t *data, size_t len,
                                            h2_gizclaw_profile_t *out_profile) {
  if (data == NULL || out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  size_t offset = 0u;
  bool has_info = false;
  while (offset < len) {
    uint64_t key = 0u;
    int rc = read_varint(data, len, &offset, &key);
    if (rc != H2_PAL_OK || key == 0u)
      return H2_PAL_ERR_FORMAT;
    if ((key >> 3u) == 1u && (key & 7u) == 2u) {
      const uint8_t *info = NULL;
      size_t info_len = 0u;
      rc = read_bytes(data, len, &offset, &info, &info_len);
      if (rc == H2_PAL_OK)
        rc = decode_device_info(info, info_len, out_profile);
      if (rc != H2_PAL_OK)
        return rc;
      has_info = true;
      continue;
    }
    rc = skip_field(data, len, &offset, (uint8_t)(key & 7u));
    if (rc != H2_PAL_OK)
      return rc;
  }
  return has_info ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static const char profile_get_tag;
static const char profile_put_name_tag;
static const char profile_put_emoji_tag;

h2_pal_result_t
h2_gizclaw_req_create_profile_get(h2_gizclaw_service_t *service,
                                  uint64_t identity, uint32_t timeout_ms,
                                  h2_gizclaw_req_t **out_request) {
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_INFO_GET, &profile_get_tag,
      (h2_gizclaw_rpc_bytes_t){0}, timeout_ms, out_request);
}

static h2_pal_result_t create_profile_put(h2_gizclaw_service_t *service,
                                          uint64_t identity,
                                          h2_gizclaw_str_t text,
                                          uint32_t timeout_ms, const void *tag,
                                          h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  uint8_t payload[H2_GIZCLAW_PROFILE_NAME_MAX_BYTES + 8u];
  size_t payload_len = 0u;
  h2_pal_result_t rc =
      (h2_pal_result_t)(tag == &profile_put_name_tag
                            ? h2_gizclaw_profile_encode_name_request(
                                  text, payload, sizeof(payload), &payload_len)
                            : h2_gizclaw_profile_encode_emoji_request(
                                  text, payload, sizeof(payload),
                                  &payload_len));
  if (rc != H2_PAL_OK)
    return rc;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_INFO_PUT, tag,
      (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_req_create_profile_put_name(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  return create_profile_put(service, identity, name, timeout_ms,
                            &profile_put_name_tag, out_request);
}

h2_pal_result_t h2_gizclaw_req_create_profile_put_emoji(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t emoji,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  return create_profile_put(service, identity, emoji, timeout_ms,
                            &profile_put_emoji_tag, out_request);
}

static h2_pal_result_t parse_profile(const h2_gizclaw_req_t *request,
                                     const void *tag,
                                     h2_gizclaw_profile_t *out_profile) {
  if (out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, tag, &response);
  if (rc == H2_PAL_OK) {
    h2_gizclaw_profile_t profile = {0};
    rc = response->result_payload_len == 0u
             ? H2_PAL_ERR_FORMAT
             : (h2_pal_result_t)h2_gizclaw_profile_decode_info_response(
                   response->result_payload, response->result_payload_len,
                   &profile);
    if (rc == H2_PAL_OK)
      *out_profile = profile;
  }
  return rc;
}

h2_pal_result_t
h2_gizclaw_resp_parse_profile_get(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_profile_t *out_profile) {
  return parse_profile(request, &profile_get_tag, out_profile);
}

h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_name(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_profile_t *out_profile) {
  return parse_profile(request, &profile_put_name_tag, out_profile);
}

h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_emoji(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_profile_t *out_profile) {
  return parse_profile(request, &profile_put_emoji_tag, out_profile);
}

static h2_pal_result_t finish_profile_rpc(h2_gizclaw_req_t *request,
                                          const void *tag,
                                          h2_gizclaw_profile_t *out_profile) {
  h2_pal_result_t rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = parse_profile(request, tag, out_profile);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *service,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_profile_t *out_profile) {
  if (out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_profile_get(service, 0u, timeout_ms, &request);
  return rc == H2_PAL_OK
             ? finish_profile_rpc(request, &profile_get_tag, out_profile)
             : rc;
}

h2_pal_result_t
h2_gizclaw_rpc_profile_put_name(h2_gizclaw_service_t *service,
                                h2_gizclaw_str_t name, uint32_t timeout_ms,
                                h2_gizclaw_profile_t *out_profile) {
  if (out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_profile_put_name(
      service, 0u, name, timeout_ms, &request);
  return rc == H2_PAL_OK
             ? finish_profile_rpc(request, &profile_put_name_tag, out_profile)
             : rc;
}

h2_pal_result_t
h2_gizclaw_rpc_profile_put_emoji(h2_gizclaw_service_t *service,
                                 h2_gizclaw_str_t emoji, uint32_t timeout_ms,
                                 h2_gizclaw_profile_t *out_profile) {
  if (out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_profile_put_emoji(
      service, 0u, emoji, timeout_ms, &request);
  return rc == H2_PAL_OK
             ? finish_profile_rpc(request, &profile_put_emoji_tag, out_profile)
             : rc;
}
