#include "h2_alipay_client.h"

#include "h2_alipay_client_internal.h"
#include "h2_sha256.h"
#include "yyjson.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct response_headers {
  h2_alipay_client_result_t* result;
  bool seen;
} response_headers_t;

void h2_alipay_client_wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

void h2_alipay_client_result_clear(h2_alipay_client_result_t* result) {
  if (result != NULL) h2_alipay_client_wipe(result, sizeof(*result));
}

void h2_alipay_client_fingerprint(const h2_alipay_client_config_t* config,
                                  uint8_t out[32]) {
  const char* values[] = {
      config->identity.sn,    config->identity.imei, config->identity.mac,
      config->identity.model, config->company,
  };
  h2_sha256_t digest;
  h2_sha256_init(&digest);
  for (size_t index = 0u; index < sizeof(values) / sizeof(*values); ++index) {
    h2_sha256_update(&digest, (const uint8_t*)values[index],
                     strlen(values[index]));
    h2_sha256_update(&digest, (const uint8_t*)"", 1u);
  }
  h2_sha256_finish(&digest, out);
}

int h2_alipay_client_finish_namespace(h2_pal_pref_namespace_t* namespace,
                                      int result) {
  int closed = namespace->close != NULL ? namespace->close(namespace)
                                        : H2_PAL_ERR_UNSUPPORTED;
  result = result != H2_PAL_OK ? result : closed;
  return result <= 0 ? result : H2_PAL_ERR_IO;
}

int h2_alipay_client_load_state(const h2_alipay_client_config_t* config,
                                client_state_t* state) {
  h2_alipay_client_fingerprint(config, state->fingerprint);
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (rc != H2_PAL_OK) return rc < 0 ? rc : H2_PAL_ERR_IO;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  void* raw = NULL;
  size_t length = 0u;
  rc = namespace->get_blob != NULL
           ? namespace->get_blob(namespace, config->mem, "context", &raw,
                                 &length)
           : H2_PAL_ERR_UNSUPPORTED;
  rc = h2_alipay_client_finish_namespace(namespace, rc);
  if (rc == H2_PAL_OK) {
    const uint8_t* data = raw;
    if (data == NULL || length < 54u || length > 184u ||
        memcmp(data, "APC1", 4u) != 0 || data[52] != length - 53u) {
      rc = H2_PAL_ERR_FORMAT;
    } else if (memcmp(data + 4u, state->fingerprint, 32u) != 0) {
      rc = H2_PAL_ERR_INVALID_STATE;
    } else {
      for (size_t index = 36u; index < 52u; ++index)
        if (!((data[index] >= '0' && data[index] <= '9') ||
              (data[index] >= 'A' && data[index] <= 'F')))
          rc = H2_PAL_ERR_FORMAT;
      for (size_t index = 53u; index < length; ++index)
        if (data[index] < 0x21u || data[index] > 0x7eu) rc = H2_PAL_ERR_FORMAT;
      if (rc == H2_PAL_OK) {
        memcpy(state->biztid, data + 36u, 16u);
        memcpy(state->device_name, data + 53u, length - 53u);
      }
    }
  }
  h2_pal_mem_free(config->mem, raw);
  return rc;
}

static int save_state(const h2_alipay_client_config_t* config,
                      const client_state_t* state) {
  uint8_t data[184] = {0};
  size_t name_length = strlen(state->device_name);
  if (name_length == 0u || name_length > 131u) return H2_PAL_ERR_INVALID_STATE;
  memcpy(data, "APC1", 4u);
  memcpy(data + 4u, state->fingerprint, 32u);
  memcpy(data + 36u, state->biztid, 16u);
  data[52] = (uint8_t)name_length;
  memcpy(data + 53u, state->device_name, name_length);
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc < 0 ? rc : H2_PAL_ERR_IO;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (namespace->set_blob == NULL || namespace->commit == NULL)
    return h2_alipay_client_finish_namespace(namespace, H2_PAL_ERR_UNSUPPORTED);
  rc = namespace->set_blob(namespace, "context", data, 53u + name_length);
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  return h2_alipay_client_finish_namespace(namespace, rc);
}

static bool header_is(h2_pal_http_str_t value, const char* expected) {
  size_t length = strlen(expected);
  if (value.len != length) return false;
  for (size_t index = 0u; index < length; ++index) {
    unsigned char byte = (unsigned char)value.data[index];
    if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    if (byte != (unsigned char)expected[index]) return false;
  }
  return true;
}

static int response_header(void* user, const h2_pal_http_request_t* request,
                           h2_pal_http_str_t name, h2_pal_http_str_t value) {
  (void)request;
  response_headers_t* headers = user;
  if (!header_is(name, "result-status")) return H2_PAL_OK;
  if (headers->seen || value.len == 0u || value.len > 10u)
    return H2_PAL_ERR_FORMAT;
  int status = 0;
  for (size_t index = 0u; index < value.len; ++index) {
    unsigned char byte = (unsigned char)value.data[index];
    if (byte < '0' || byte > '9' || status > (INT_MAX - (byte - '0')) / 10)
      return H2_PAL_ERR_FORMAT;
    status = status * 10 + byte - '0';
  }
  headers->seen = true;
  headers->result->rpc_status = status;
  return H2_PAL_OK;
}

int h2_alipay_client_post(const h2_alipay_client_config_t* config,
                          client_workspace_t* workspace,
                          h2_alipay_client_result_t* result,
                          const char* operation, bool binary,
                          const uint8_t* body, size_t body_length,
                          size_t* response_length) {
  *response_length = 0u;
  uint64_t utc_ms = 0u;
  int rc = h2_pal_time_get_wall_ms(config->time, &utc_ms);
  if (rc != H2_PAL_OK) return rc;
  char timestamp[24], rpc_id[16], signature[65];
  rc = h2_alipay_protocol_timestamp(utc_ms, timestamp);
  if (rc != H2_PAL_OK) return rc;
  (void)snprintf(rpc_id, sizeof(rpc_id), "%u", result->requests + 1u);
  h2_pal_http_header_t headers[] = {
      {{"Operation-Type", 14u}, {operation, strlen(operation)}},
      {{"AppId", 5u},
       {"AlipayInside_IoTPay_NET", sizeof("AlipayInside_IoTPay_NET") - 1u}},
      {{"Version", 7u}, {"2", 1u}},
      {{"RpcId", 5u}, {rpc_id, strlen(rpc_id)}},
      {{"Ts", 2u}, {timestamp, strlen(timestamp)}},
      {{"Content-Type", 12u},
       {binary ? "application/protobuf" : "application/json",
        binary ? 20u : 16u}},
      {{"Sign", 4u}, {signature, 64u}},
      {{"signType", 8u}, {"0", 1u}},
      {{"did", 3u},
       {workspace->state.device_name, strlen(workspace->state.device_name)}},
  };
  if (!binary) {
    uint8_t key[32];
    rc = h2_alipay_protocol_signing_key(&config->identity,
                                        workspace->state.device_name, key);
    size_t length = 0u;
    if (rc == H2_PAL_OK)
      rc = h2_alipay_protocol_canonical(operation, timestamp, false, body,
                                        body_length, workspace->canonical,
                                        sizeof(workspace->canonical), &length);
    if (rc == H2_PAL_OK)
      rc = h2_alipay_protocol_signature(
          key, (const uint8_t*)workspace->canonical, length, signature);
    h2_alipay_client_wipe(key, sizeof(key));
    h2_alipay_client_wipe(workspace->canonical, sizeof(workspace->canonical));
    if (rc != H2_PAL_OK) return rc;
  }
  response_headers_t observed = {.result = result};
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {config->gateway_url, strlen(config->gateway_url)},
      .headers = headers,
      .header_count = binary ? 6u : 9u,
      .body = body,
      .body_len = body_length,
      .response_header_cb = response_header,
      .response_header_user = &observed,
      .timeout_ms = 45000,
      .connect_timeout_ms = 15000,
      .retry_count = 0,
      .response_buf = workspace->response,
      .response_buf_cap = sizeof(workspace->response),
      .allocator = config->mem,
      .cancel_cb = config->cancel_cb,
      .cancel_user = config->cancel_user,
  };
  h2_pal_http_response_t response = {0};
  result->rpc_status = -1;
  result->http_status = 0;
  ++result->requests;
  rc = h2_pal_http_request(config->http, &request, &response);
  if (rc == H2_PAL_OK && h2_pal_http_request_is_canceled(&request))
    rc = H2_PAL_ERR_CLOSED;
  result->http_status = response.status_code;
  if (rc == H2_PAL_OK && (!observed.seen || response.status_code != 200 ||
                          result->rpc_status != 1000))
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    if (response.body_len > sizeof(workspace->response))
      rc = H2_PAL_ERR_NO_SPACE;
    else
      *response_length = response.body_len;
  }
  h2_pal_http_response_free(config->http, &response);
  h2_alipay_client_wipe(signature, sizeof(signature));
  return rc;
}

typedef union json_block {
#if defined(_MSC_VER) && !defined(__clang__)
  long double alignment;
  long long integer_alignment;
  void* pointer_alignment;
#else
  max_align_t alignment;
#endif
  size_t size;
} json_block_t;

void* h2_alipay_client_json_alloc(void* user, size_t size) {
  if (size > SIZE_MAX - sizeof(json_block_t)) return NULL;
  json_block_t* block = h2_pal_mem_alloc(user, sizeof(*block) + size);
  if (block == NULL) return NULL;
  block->size = size;
  return block + 1;
}
void h2_alipay_client_json_free(void* user, void* data) {
  if (data == NULL) return;
  json_block_t* block = (json_block_t*)data - 1;
  h2_alipay_client_wipe(block, sizeof(*block) + block->size);
  h2_pal_mem_free(user, block);
}

yyjson_val* h2_alipay_client_json_member(yyjson_val* root, const char* name) {
  if (name == NULL) return NULL;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
  yyjson_val* key;
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char* text = yyjson_get_str(key);
    size_t index = 0u;
    for (;;) {
      unsigned left = (unsigned char)text[index];
      unsigned right = (unsigned char)name[index];
      if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
      if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
      if (left != right) break;
      if (left == 0u) return yyjson_obj_iter_get_val(key);
      ++index;
    }
  }
  return NULL;
}

void* h2_alipay_client_json_realloc(void* user, void* previous, size_t old_size,
                                    size_t size) {
  (void)old_size;
  void* next = h2_alipay_client_json_alloc(user, size);
  if (next == NULL) return NULL;
  if (previous != NULL) {
    json_block_t* block = (json_block_t*)previous - 1;
    memcpy(next, previous, block->size < size ? block->size : size);
    h2_alipay_client_json_free(user, previous);
  }
  return next;
}

static int read_token(const h2_alipay_client_config_t* config,
                      client_workspace_t* workspace, size_t length,
                      char token[129]) {
  yyjson_alc allocator = {h2_alipay_client_json_alloc,
                          h2_alipay_client_json_realloc,
                          h2_alipay_client_json_free, (void*)config->mem};
  yyjson_read_err error;
  yyjson_doc* document = yyjson_read_opts((char*)workspace->response, length,
                                          0u, &allocator, &error);
  if (document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(document);
  yyjson_val* success = yyjson_obj_get(root, "success");
  yyjson_val* value = yyjson_obj_get(root, "token");
  int rc = H2_PAL_ERR_FORMAT;
  size_t token_length = yyjson_get_len(value);
  if (yyjson_is_obj(root) && yyjson_is_true(success) && yyjson_is_str(value) &&
      token_length > 0u && token_length <= 128u &&
      strlen(yyjson_get_str(value)) == token_length) {
    bool duplicate = false;
    unsigned token_fields = 0u, success_fields = 0u;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
      if (yyjson_equals_str(key, "token")) duplicate |= ++token_fields > 1u;
      if (yyjson_equals_str(key, "success")) duplicate |= ++success_fields > 1u;
    }
    if (!duplicate) {
      memcpy(token, yyjson_get_str(value), token_length + 1u);
      rc = H2_PAL_OK;
    }
  }
  yyjson_doc_free(document);
  return rc;
}

static bool gateway_valid(const char* url) {
  if (url == NULL || strncmp(url, "https://", 8u) != 0) return false;
  const char* path = strchr(url + 8u, '/');
  if (path == NULL || path == url + 8u || path[1] == '\0') return false;
  for (const char* cursor = url + 8u; *cursor != '\0'; ++cursor) {
    unsigned char byte = (unsigned char)*cursor;
    if (byte <= 0x20u || byte >= 0x7fu || byte == '@' || byte == '?' ||
        byte == '#' || byte == '\\')
      return false;
  }
  return true;
}

h2_pal_result_t h2_alipay_client_validate_config(
    const h2_alipay_client_config_t* config) {
  if (config == NULL || config->mem == NULL || config->time == NULL ||
      config->http == NULL || config->pref == NULL || config->crypto == NULL ||
      config->company == NULL || config->pref_namespace == NULL ||
      config->pref_namespace[0] == '\0' || config->company[0] == '\0' ||
      strlen(config->company) > 15u || !gateway_valid(config->gateway_url))
    return H2_PAL_ERR_INVALID_ARG;
  if (config->product_type != 3u) return H2_PAL_ERR_UNSUPPORTED;
  if ((config->legacy_pref == NULL) !=
          (config->legacy_pref_namespace == NULL) ||
      (config->legacy_pref_namespace != NULL &&
       config->legacy_pref_namespace[0] == '\0'))
    return H2_PAL_ERR_INVALID_ARG;
  for (const char* p = config->company; *p != '\0'; ++p)
    if ((unsigned char)*p < 0x20u || (unsigned char)*p > 0x7eu || *p == '#')
      return H2_PAL_ERR_INVALID_ARG;
  size_t length = 0u;
  int rc =
      h2_alipay_protocol_registration(&config->identity, NULL, 0u, &length);
  return rc == H2_PAL_ERR_NO_SPACE ? H2_PAL_OK : rc;
}

h2_pal_result_t h2_alipay_client_get_binding_code(
    const h2_alipay_client_config_t* config, h2_alipay_client_result_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_client_result_clear(out);
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_client_binding_t binding;
  rc = h2_alipay_client_binding_snapshot(config, &binding);
  if (rc != H2_PAL_OK) return rc;
  if (binding.state != H2_ALIPAY_CLIENT_UNBOUND)
    return H2_PAL_ERR_INVALID_STATE;
  size_t required = 0u;
  client_workspace_t* workspace =
      h2_pal_mem_alloc(config->mem, sizeof(*workspace));
  if (workspace == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(workspace, 0, sizeof(*workspace));
  char* body = NULL;
  size_t body_length = 0u, response_length = 0u;
  out->stage = H2_ALIPAY_CLIENT_STORAGE;
  rc = h2_alipay_client_load_state(config, &workspace->state);
  bool fresh = rc == H2_PAL_ERR_NOT_FOUND;
  if (rc != H2_PAL_OK && !fresh) goto done;
  if (fresh) {
    out->stage = H2_ALIPAY_CLIENT_RANDOM;
    uint8_t entropy[4];
    uint64_t utc_ms = 0u;
    rc = h2_pal_time_get_wall_ms(config->time, &utc_ms);
    if (rc == H2_PAL_OK)
      rc = h2_pal_crypto_random(config->crypto, entropy, sizeof(entropy));
    if (rc != H2_PAL_OK) goto done;
    uint32_t random = (uint32_t)entropy[0] | ((uint32_t)entropy[1] << 8u) |
                      ((uint32_t)entropy[2] << 16u) |
                      ((uint32_t)entropy[3] << 24u);
    (void)snprintf(workspace->state.biztid, sizeof(workspace->state.biztid),
                   "%08" PRIX32 "%08" PRIX32, random,
                   (uint32_t)(utc_ms / 1000u));
  }
  out->stage = H2_ALIPAY_CLIENT_KEYPAIR;
  rc = h2_pal_crypto_p256_keypair_generate(config->crypto, &workspace->keypair);
  if (rc != H2_PAL_OK) goto done;
  out->stage = H2_ALIPAY_CLIENT_PUBLIC_KEY;
  rc = h2_pal_crypto_p256_public_key_uncompress(
      config->crypto, &workspace->keypair.public_key, workspace->public_key);
  if (rc != H2_PAL_OK) goto done;
  out->stage = H2_ALIPAY_CLIENT_ENCODE;
  rc = h2_alipay_protocol_prepare_bind(
      config->mem, &config->identity, config->company, workspace->state.biztid,
      config->feature, config->compile_timestamp_s, workspace->public_key + 1u,
      &body, &body_length);
  if (rc != H2_PAL_OK) goto done;
  if (fresh) {
    uint8_t registration[512];
    out->stage = H2_ALIPAY_CLIENT_REGISTER;
    rc = h2_alipay_protocol_registration(&config->identity, registration,
                                         sizeof(registration), &required);
    if (rc == H2_PAL_OK)
      rc = h2_alipay_client_post(config, workspace, out,
                                 "iothub.internal.device.active", true,
                                 registration, required, &response_length);
    if (rc == H2_PAL_OK)
      rc = h2_alipay_protocol_device_name(workspace->response, response_length,
                                          workspace->state.device_name,
                                          sizeof(workspace->state.device_name));
    if (rc == H2_PAL_OK) {
      out->registered = true;
      out->stage = H2_ALIPAY_CLIENT_COMMIT;
      rc = save_state(config, &workspace->state);
    }
    if (rc != H2_PAL_OK) goto done;
  }
  out->stage = H2_ALIPAY_CLIENT_PREPARE_BIND;
  rc = h2_alipay_client_post(
      config, workspace, out,
      "alipay.mobile.alideviceinfo.changecode.prepareBind", false,
      (const uint8_t*)body, body_length, &response_length);
  if (rc == H2_PAL_OK) {
    out->stage = H2_ALIPAY_CLIENT_TOKEN;
    rc = read_token(config, workspace, response_length, out->token);
  }
  if (rc == H2_PAL_OK) {
    out->stage = H2_ALIPAY_CLIENT_QR;
    rc = h2_alipay_protocol_binding_code(&config->identity, config->company,
                                         out->token, config->feature,
                                         config->compile_timestamp_s, out->code,
                                         sizeof(out->code), &out->length);
  }
  if (rc == H2_PAL_OK) out->private_key = workspace->keypair.private_key;
done:
  if (body != NULL) h2_alipay_client_wipe(body, body_length);
  h2_pal_mem_free(config->mem, body);
  h2_alipay_client_wipe(workspace, sizeof(*workspace));
  h2_pal_mem_free(config->mem, workspace);
  if (rc != H2_PAL_OK) {
    h2_alipay_client_wipe(out->code, sizeof(out->code));
    h2_alipay_client_wipe(out->token, sizeof(out->token));
    h2_alipay_client_wipe(&out->private_key, sizeof(out->private_key));
    out->length = 0u;
  }
  return rc;
}
