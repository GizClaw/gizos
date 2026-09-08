#include "h2_gizclaw_app_config.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include <limits.h>
#include <string.h>

static const char list_tag, get_tag;

/* Callback layouts preserve byte lengths (including embedded NUL in values)
 * and avoid placing the SDK's 4 KiB fixed response arrays on worker stacks.
 * Field tags and wire types match the SDK's AppConfig response messages. */
typedef struct config_list_wire {
  pb_callback_t keys, next_cursor, runtime_profile_name,
      runtime_profile_revision;
  bool has_next;
} config_list_wire;
#define config_list_wire_FIELDLIST(X, a)                                       \
  X(a, CALLBACK, REPEATED, STRING, keys, 1)                                    \
  X(a, STATIC, SINGULAR, BOOL, has_next, 2)                                    \
  X(a, CALLBACK, OPTIONAL, STRING, next_cursor, 3)                             \
  X(a, CALLBACK, SINGULAR, STRING, runtime_profile_name, 4)                    \
  X(a, CALLBACK, SINGULAR, STRING, runtime_profile_revision, 5)
#define config_list_wire_CALLBACK pb_default_field_callback
#define config_list_wire_DEFAULT NULL
PB_BIND(config_list_wire, config_list_wire, AUTO)

typedef struct config_get_wire {
  pb_callback_t value, runtime_profile_name, runtime_profile_revision;
} config_get_wire;
#define config_get_wire_FIELDLIST(X, a)                                        \
  X(a, CALLBACK, SINGULAR, STRING, value, 1)                                   \
  X(a, CALLBACK, SINGULAR, STRING, runtime_profile_name, 2)                    \
  X(a, CALLBACK, SINGULAR, STRING, runtime_profile_revision, 3)
#define config_get_wire_CALLBACK pb_default_field_callback
#define config_get_wire_DEFAULT NULL
PB_BIND(config_get_wire, config_get_wire, AUTO)

static bool key_valid(h2_gizclaw_str_t key) {
  if (key.data == NULL || key.len == 0 ||
      key.len > H2_GIZCLAW_APP_CONFIG_KEY_MAX_BYTES)
    return false;
  for (size_t i = 0; i < key.len; ++i) {
    char c = key.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (i > 0 && i + 1 < key.len && (c == '.' || c == '-') &&
           key.data[i - 1] != '.' && key.data[i - 1] != '-')))
      return false;
  }
  return true;
}

static h2_pal_result_t create(h2_gizclaw_service_t *service, uint64_t identity,
                              const pb_msgdesc_t *fields, const void *message,
                              bool list, uint32_t timeout_ms,
                              h2_gizclaw_req_t **out) {
  if (out != NULL)
    *out = NULL;
  if (service == NULL || out == NULL || timeout_ms == 0 ||
      timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t data[gizclaw_rpc_v1_AppConfigListRequest_size];
  pb_ostream_t stream = pb_ostream_from_buffer(data, sizeof(data));
  if (!pb_encode(&stream, fields, message))
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_req_create_rpc_internal(
      service, identity,
      list ? H2_GIZCLAW_RPC_SERVER_APP_CONFIG_LIST
           : H2_GIZCLAW_RPC_SERVER_APP_CONFIG_GET,
      list ? &list_tag : &get_tag,
      (h2_gizclaw_rpc_bytes_t){data, stream.bytes_written}, timeout_ms, out);
}

h2_pal_result_t h2_gizclaw_req_create_app_config_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (limit == 0 || limit > H2_GIZCLAW_APP_CONFIG_PAGE_MAX_ITEMS ||
      cursor.len > H2_GIZCLAW_APP_CONFIG_CURSOR_MAX_BYTES ||
      (cursor.len &&
       (cursor.data == NULL || memchr(cursor.data, 0, cursor.len))))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_AppConfigListRequest msg =
      gizclaw_rpc_v1_AppConfigListRequest_init_zero;
  msg.has_cursor = cursor.len != 0;
  if (cursor.len)
    memcpy(msg.cursor, cursor.data, cursor.len);
  msg.has_limit = true;
  msg.limit = (int64_t)limit;
  return create(service, identity, gizclaw_rpc_v1_AppConfigListRequest_fields,
                &msg, true, timeout_ms, out_request);
}
h2_pal_result_t h2_gizclaw_req_create_app_config_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t key,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!key_valid(key))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_AppConfigGetRequest msg =
      gizclaw_rpc_v1_AppConfigGetRequest_init_zero;
  memcpy(msg.key, key.data, key.len);
  return create(service, identity, gizclaw_rpc_v1_AppConfigGetRequest_fields,
                &msg, false, timeout_ms, out_request);
}

typedef struct text_context {
  const h2_pal_mem_api_t *mem;
  char **out;
  size_t max, *len;
} text_context;
static bool decode_text(pb_istream_t *s, const pb_field_t *field, void **arg) {
  (void)field;
  text_context *c = *arg;
  size_t n = s->bytes_left;
  if (n > c->max)
    return false;
  char *p = h2_pal_mem_alloc(c->mem, n + 1);
  if (p == NULL || !pb_read(s, (pb_byte_t *)p, n))
    return false;
  if (c->len == NULL && memchr(p, 0, n))
    return false;
  p[n] = 0;
  *c->out = p;
  if (c->len)
    *c->len = n;
  return true;
}
static pb_callback_t text_callback(text_context *c) {
  return (pb_callback_t){.funcs.decode = decode_text, .arg = c};
}
typedef struct keys_context {
  const h2_pal_mem_api_t *mem;
  h2_gizclaw_app_config_page_t *page;
  size_t limit;
} keys_context;
static bool decode_key(pb_istream_t *s, const pb_field_t *field, void **arg) {
  keys_context *c = *arg;
  if (c->page->count >= c->limit)
    return false;
  char *key = NULL;
  text_context t = {c->mem, &key, H2_GIZCLAW_APP_CONFIG_KEY_MAX_BYTES, NULL};
  void *p = &t;
  if (!decode_text(s, field, &p) ||
      !key_valid((h2_gizclaw_str_t){key, strlen(key)}))
    return false;
  for (size_t i = 0; i < c->page->count; ++i)
    if (strcmp(c->page->keys[i], key) == 0)
      return false;
  c->page->keys[c->page->count++] = key;
  return true;
}
static bool identity_valid(const char *name, const char *revision) {
  return name != NULL && name[0] != 0 && revision != NULL && revision[0] != 0;
}
h2_pal_result_t h2_gizclaw_resp_parse_app_config_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_app_config_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &list_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_AppConfigListRequest req =
      gizclaw_rpc_v1_AppConfigListRequest_init_zero;
  pb_istream_t s = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&s, gizclaw_rpc_v1_AppConfigListRequest_fields, &req) ||
      req.limit < 1 || req.limit > H2_GIZCLAW_APP_CONFIG_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_app_config_page_t result = {0};
  result.keys =
      h2_pal_mem_alloc(&arena.allocator, (size_t)req.limit * sizeof(char *));
  keys_context keys = {&arena.allocator, &result, (size_t)req.limit};
  text_context cursor = {&arena.allocator, &result.next_cursor, 171, NULL};
  text_context name = {&arena.allocator, &result.runtime_profile_name, 255,
                       NULL};
  text_context revision = {&arena.allocator, &result.runtime_profile_revision,
                           64, NULL};
  config_list_wire msg = {.keys = {.funcs.decode = decode_key, .arg = &keys},
                          .next_cursor = text_callback(&cursor),
                          .runtime_profile_name = text_callback(&name),
                          .runtime_profile_revision = text_callback(&revision)};
  s = pb_istream_from_buffer(response->result_payload,
                             response->result_payload_len);
  bool ok = result.keys != NULL && pb_decode(&s, &config_list_wire_msg, &msg);
  result.has_next = msg.has_next;
  ok = ok &&
       identity_valid(result.runtime_profile_name,
                      result.runtime_profile_revision) &&
       (!result.has_next || (result.count > 0 && result.next_cursor != NULL &&
                             result.next_cursor[0] != 0 &&
                             strcmp(result.next_cursor, req.cursor) != 0));
  rc = h2_gizclaw_resp_arena_end(&arena, ok ? H2_PAL_OK : H2_PAL_ERR_FORMAT);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_app_config_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_app_config_value_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_app_config_value_t result = {0};
  text_context value = {&arena.allocator, &result.value.data, 4096,
                        &result.value.len};
  text_context name = {&arena.allocator, &result.runtime_profile_name, 255,
                       NULL};
  text_context revision = {&arena.allocator, &result.runtime_profile_revision,
                           64, NULL};
  config_get_wire msg = {.value = text_callback(&value),
                         .runtime_profile_name = text_callback(&name),
                         .runtime_profile_revision = text_callback(&revision)};
  pb_istream_t s = pb_istream_from_buffer(response->result_payload,
                                          response->result_payload_len);
  bool ok = pb_decode(&s, &config_get_wire_msg, &msg) &&
            identity_valid(result.runtime_profile_name,
                           result.runtime_profile_revision);
  if (ok && result.value.data == NULL) {
    result.value.data = h2_pal_mem_alloc(&arena.allocator, 1);
    if (result.value.data)
      result.value.data[0] = 0;
  }
  rc = h2_gizclaw_resp_arena_end(&arena, ok ? H2_PAL_OK : H2_PAL_ERR_FORMAT);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_app_config_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_app_config_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_app_config_list(
      service, 0, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_app_config_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
h2_pal_result_t
h2_gizclaw_rpc_app_config_get(h2_gizclaw_service_t *service,
                              h2_gizclaw_str_t key, uint32_t timeout_ms,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_app_config_value_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_app_config_get(
      service, 0, key, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_app_config_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
