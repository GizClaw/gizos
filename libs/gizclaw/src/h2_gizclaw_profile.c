#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_profile_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include <stdatomic.h>
#include <string.h>

struct h2_gizclaw_profile_request {
  h2_gizclaw_async_rpc_t *rpc;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_profile_completion_fn completion;
  void *completion_user;
  h2_gizclaw_profile_t profile;
  atomic_bool terminal;
};

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

static int encode_profile_text_request(h2_gizclaw_str_t text,
                                       size_t max_bytes, uint8_t field_key,
                                       uint8_t *out, size_t capacity,
                                       size_t *out_len) {
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

int h2_gizclaw_profile_encode_name_request(h2_gizclaw_str_t name,
                                           uint8_t *out, size_t capacity,
                                           size_t *out_len) {
  return encode_profile_text_request(
      name, H2_GIZCLAW_PROFILE_NAME_MAX_BYTES, 0x0au, out, capacity, out_len);
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
  while (offset < len) {
    uint64_t key = 0u;
    int rc = read_varint(data, len, &offset, &key);
    if (rc != H2_PAL_OK || key == 0u)
      return H2_PAL_ERR_FORMAT;
    if ((key >> 3u) == 1u && (key & 7u) == 2u) {
      const uint8_t *info = NULL;
      size_t info_len = 0u;
      rc = read_bytes(data, len, &offset, &info, &info_len);
      return rc == H2_PAL_OK ? decode_device_info(info, info_len, out_profile)
                             : rc;
    }
    rc = skip_field(data, len, &offset, (uint8_t)(key & 7u));
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_ERR_FORMAT;
}

static int client_profile_call(h2_gizclaw_client_t *client,
                               h2_gizclaw_rpc_method_t method,
                               h2_gizclaw_rpc_bytes_t payload,
                               h2_gizclaw_profile_t *out_profile) {
  if (client == NULL || out_profile == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_profile, 0, sizeof(*out_profile));
  h2_gizclaw_rpc_response_t response = {0};
  int rc = h2_gizclaw_client_rpc_call(client, method, payload, &response);
  if (rc == H2_PAL_OK && response.has_error)
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_profile_decode_info_response(
        response.result_payload, response.result_payload_len, out_profile);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return rc;
}

int h2_gizclaw_client_profile_get(h2_gizclaw_client_t *client,
                                  h2_gizclaw_profile_t *out_profile) {
  return client_profile_call(client, H2_GIZCLAW_RPC_SERVER_INFO_GET,
                             (h2_gizclaw_rpc_bytes_t){0}, out_profile);
}

int h2_gizclaw_client_profile_put_name(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_profile_t *out_profile) {
  uint8_t payload[H2_GIZCLAW_PROFILE_NAME_MAX_BYTES + 8u];
  size_t payload_len = 0u;
  const int rc = h2_gizclaw_profile_encode_name_request(
      name, payload, sizeof(payload), &payload_len);
  if (rc != H2_PAL_OK) {
    if (out_profile != NULL)
      memset(out_profile, 0, sizeof(*out_profile));
    return rc;
  }
  return client_profile_call(
      client, H2_GIZCLAW_RPC_SERVER_INFO_PUT,
      (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
      out_profile);
}

int h2_gizclaw_client_profile_put_emoji(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t emoji,
                                        h2_gizclaw_profile_t *out_profile) {
  uint8_t payload[H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES + 8u];
  size_t payload_len = 0u;
  const int rc = h2_gizclaw_profile_encode_emoji_request(
      emoji, payload, sizeof(payload), &payload_len);
  if (rc != H2_PAL_OK) {
    if (out_profile != NULL)
      memset(out_profile, 0, sizeof(*out_profile));
    return rc;
  }
  return client_profile_call(
      client, H2_GIZCLAW_RPC_SERVER_INFO_PUT,
      (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
      out_profile);
}

static void profile_rpc_complete(
    void *user, h2_gizclaw_async_rpc_t *rpc,
    const h2_gizclaw_operation_result_t *operation_result,
    const h2_gizclaw_rpc_response_t *response) {
  (void)rpc;
  h2_gizclaw_profile_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK) {
    if (response == NULL || response->has_error) {
      result.result = H2_PAL_ERR_IO;
    } else {
      result.result = (h2_pal_result_t)h2_gizclaw_profile_decode_info_response(
          response->result_payload, response->result_payload_len,
          &request->profile);
    }
  }
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, &result,
                      result.result == H2_PAL_OK ? &request->profile : NULL);
}

static h2_pal_result_t submit_profile_request(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t payload,
    uint32_t timeout_ms, h2_gizclaw_profile_completion_fn completion,
    void *user, h2_gizclaw_profile_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || timeout_ms == 0u || completion == NULL ||
      out_request == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_profile_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->completion = completion;
  request->completion_user = user;
  const h2_pal_result_t rc = h2_gizclaw_service_rpc_call_async(
      service, identity, method, payload, timeout_ms, profile_rpc_complete,
      request, &request->rpc);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_profile_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_profile_completion_fn completion, void *user,
    h2_gizclaw_profile_request_t **out_request) {
  return submit_profile_request(
      service, identity, H2_GIZCLAW_RPC_SERVER_INFO_GET,
      (h2_gizclaw_rpc_bytes_t){0}, timeout_ms, completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_profile_put_name_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_profile_completion_fn completion,
    void *user, h2_gizclaw_profile_request_t **out_request) {
  uint8_t payload[H2_GIZCLAW_PROFILE_NAME_MAX_BYTES + 8u];
  size_t payload_len = 0u;
  const h2_pal_result_t rc = (h2_pal_result_t)
      h2_gizclaw_profile_encode_name_request(name, payload, sizeof(payload),
                                             &payload_len);
  if (rc != H2_PAL_OK) {
    if (out_request != NULL)
      *out_request = NULL;
    return rc;
  }
  return submit_profile_request(
      service, identity, H2_GIZCLAW_RPC_SERVER_INFO_PUT,
      (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
      timeout_ms, completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_profile_put_emoji_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t emoji,
    uint32_t timeout_ms, h2_gizclaw_profile_completion_fn completion,
    void *user, h2_gizclaw_profile_request_t **out_request) {
  uint8_t payload[H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES + 8u];
  size_t payload_len = 0u;
  const h2_pal_result_t rc = (h2_pal_result_t)
      h2_gizclaw_profile_encode_emoji_request(emoji, payload, sizeof(payload),
                                              &payload_len);
  if (rc != H2_PAL_OK) {
    if (out_request != NULL)
      *out_request = NULL;
    return rc;
  }
  return submit_profile_request(
      service, identity, H2_GIZCLAW_RPC_SERVER_INFO_PUT,
      (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
      timeout_ms, completion, user, out_request);
}

h2_pal_result_t
h2_gizclaw_profile_request_cancel(h2_gizclaw_profile_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_async_rpc_cancel(request->rpc);
}

void h2_gizclaw_profile_request_release(
    h2_gizclaw_profile_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    return;
  }
  h2_gizclaw_async_rpc_release(request->rpc);
  h2_pal_mem_free(request->allocator, request);
}
