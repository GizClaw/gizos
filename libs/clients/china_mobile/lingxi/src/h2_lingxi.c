#include "h2_lingxi.h"

#include <limits.h>
#include <string.h>

typedef struct builder {
  const h2_pal_json_api_t* json;
  h2_pal_json_document_t* document;
  h2_pal_json_value_t* root;
  h2_pal_result_t result;
} builder_t;

static void wipe(void* data, size_t len) {
  volatile uint8_t* bytes = data;
  while (len-- != 0u) *bytes++ = 0u;
}

void h2_lingxi_device_info_clear(h2_lingxi_device_info_t* info) {
  if (info != NULL) wipe(info, sizeof(*info));
}

static size_t bounded_length(const char* text, size_t capacity) {
  size_t len = 0u;
  if (text != NULL)
    while (len < capacity && text[len] != '\0') ++len;
  return len;
}

static bool text_valid(const char* text, size_t capacity) {
  const size_t length = bounded_length(text, capacity);
  return text != NULL && length != 0u && length < capacity;
}

static bool origin_valid(const h2_lingxi_config_t* config, size_t* out_len) {
  const char* url = config->base_url;
  size_t len = bounded_length(url, H2_LINGXI_BASE_URL_CAPACITY);
  if (url == NULL || len == 0u || len == H2_LINGXI_BASE_URL_CAPACITY)
    return false;
  while (len != 0u && url[len - 1u] == '/') --len;
  size_t begin = 0u;
  bool plaintext = false;
  if (len > 8u && memcmp(url, "https://", 8u) == 0)
    begin = 8u;
  else if (config->allow_plaintext_loopback && len > 7u &&
           memcmp(url, "http://", 7u) == 0) {
    begin = 7u;
    plaintext = true;
  } else
    return false;
  for (size_t i = begin; i < len; ++i) {
    const unsigned char ch = (unsigned char)url[i];
    if (ch <= 32u || ch >= 127u || ch == '/' || ch == '@' || ch == '?' ||
        ch == '#' || ch == '\\')
      return false;
  }
  if (plaintext) {
    static const char* const local[] = {"127.0.0.1", "localhost", "[::1]"};
    bool allowed = false;
    for (size_t i = 0u; i < sizeof(local) / sizeof(local[0]); ++i) {
      const size_t host_len = strlen(local[i]);
      if (len - begin >= host_len &&
          memcmp(url + begin, local[i], host_len) == 0 &&
          (len - begin == host_len || url[begin + host_len] == ':')) {
        allowed = true;
        if (len - begin != host_len) {
          size_t at = begin + host_len + 1u;
          if (at == len) return false;
          for (; at < len; ++at)
            if (url[at] < '0' || url[at] > '9') return false;
        }
        break;
      }
    }
    if (!allowed) return false;
  }
  *out_len = len;
  return true;
}

static bool config_valid(const h2_lingxi_config_t* config, size_t* base_len) {
  if (config == NULL || config->allocator == NULL ||
      config->allocator->vtable == NULL ||
      config->allocator->vtable->alloc == NULL ||
      config->allocator->vtable->free == NULL || config->http == NULL ||
      config->http->vtable == NULL || config->http->vtable->request == NULL ||
      config->http->vtable->response_free == NULL || config->json == NULL ||
      config->json->vtable == NULL || config->timeout_ms == 0u ||
      config->timeout_ms > INT_MAX ||
      config->json_limits.max_document_bytes == 0u ||
      config->json_limits.max_document_bytes >
          H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES ||
      config->json_limits.max_depth == 0u ||
      config->json_limits.max_depth > H2_PAL_JSON_DEFAULT_MAX_DEPTH ||
      config->json_limits.max_values == 0u ||
      config->json_limits.max_values > H2_PAL_JSON_DEFAULT_MAX_VALUES ||
      !origin_valid(config, base_len))
    return false;
  const h2_pal_json_vtable_t* v = config->json->vtable;
  if (v->document_create == NULL || v->document_destroy == NULL ||
      v->document_parse == NULL || v->document_root == NULL ||
      v->document_set_root == NULL || v->document_serialize == NULL ||
      v->buffer_release == NULL || v->value_create_object == NULL ||
      v->value_create_array == NULL || v->value_create_string == NULL ||
      v->object_get == NULL || v->object_set == NULL ||
      v->array_append == NULL || v->value_type == NULL ||
      v->value_get_number == NULL || v->value_get_boolean == NULL ||
      v->value_get_string == NULL)
    return false;
  if (config->source_ip != NULL) {
    if (!text_valid(config->source_ip, H2_LINGXI_IP_CAPACITY)) return false;
    for (const char* at = config->source_ip; *at != '\0'; ++at)
      if ((unsigned char)*at <= 32u || (unsigned char)*at >= 127u) return false;
  }
  return true;
}

static bool product_valid(const h2_lingxi_product_t* product) {
  return product != NULL && text_valid(product->id, H2_LINGXI_ID_CAPACITY) &&
         text_valid(product->key, H2_LINGXI_PRODUCT_KEY_CAPACITY);
}

static void builder_create(builder_t* b, const h2_lingxi_config_t* config) {
  memset(b, 0, sizeof(*b));
  b->json = config->json;
  b->result =
      h2_pal_json_document_create(b->json, &config->json_limits, &b->document);
  if (b->result == H2_PAL_OK)
    b->result = h2_pal_json_value_create_object(b->json, b->document, &b->root);
  if (b->result == H2_PAL_OK)
    b->result = h2_pal_json_document_set_root(b->json, b->document, b->root);
}

static void add_string(builder_t* b, h2_pal_json_value_t* object,
                       const char* key, const char* text) {
  if (b->result != H2_PAL_OK) return;
  h2_pal_json_value_t* value = NULL;
  b->result = h2_pal_json_value_create_string(b->json, b->document, text,
                                              strlen(text), &value);
  if (b->result == H2_PAL_OK)
    b->result =
        h2_pal_json_object_set(b->json, object, key, strlen(key), value);
}

static void add_product(builder_t* b, const h2_lingxi_product_t* product) {
  add_string(b, b->root, "productId", product->id);
  add_string(b, b->root, "productKey", product->key);
}

static bool name_equal(h2_pal_http_str_t name, const char* lower) {
  const size_t len = strlen(lower);
  if (name.len != len) return false;
  for (size_t i = 0u; i < len; ++i) {
    char ch = name.data[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
    if (ch != lower[i]) return false;
  }
  return true;
}

static int reject_location(void* user, const h2_pal_http_request_t* request,
                           h2_pal_http_str_t name, h2_pal_http_str_t value) {
  (void)user;
  (void)request;
  return value.len != 0u && name_equal(name, "location")
             ? H2_PAL_ERR_UNAVAILABLE
             : H2_PAL_OK;
}

static h2_pal_result_t object_get(const h2_pal_json_api_t* json,
                                  const h2_pal_json_value_t* object,
                                  const char* key,
                                  h2_pal_json_value_t** value) {
  return h2_pal_json_object_get(json, object, key, strlen(key), value);
}

static h2_pal_result_t response_error(h2_pal_result_t rc) {
  return rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_INVALID_STATE ||
                 rc == H2_PAL_ERR_INVALID_ARG
             ? H2_PAL_ERR_FORMAT
             : rc;
}

static h2_pal_result_t read_code(const h2_pal_json_api_t* json,
                                 const h2_pal_json_value_t* value,
                                 int32_t* out) {
  h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
  h2_pal_result_t rc = h2_pal_json_value_type(json, value, &type);
  if (rc != H2_PAL_OK) return response_error(rc);
  if (type == H2_PAL_JSON_TYPE_NUMBER) {
    double number = 0.0;
    rc = h2_pal_json_value_get_number(json, value, &number);
    if (rc != H2_PAL_OK) return response_error(rc);
    if (!(number >= 0.0 && number <= INT32_MAX) ||
        number != (double)(int32_t)number)
      return H2_PAL_ERR_FORMAT;
    *out = (int32_t)number;
    return H2_PAL_OK;
  }
  h2_pal_json_string_view_t text = {0};
  rc = h2_pal_json_value_get_string(json, value, &text);
  if (rc != H2_PAL_OK) return response_error(rc);
  if (text.len == 0u || text.len > 10u) return H2_PAL_ERR_FORMAT;
  uint32_t number = 0u;
  for (size_t i = 0u; i < text.len; ++i) {
    if (text.data[i] < '0' || text.data[i] > '9') return H2_PAL_ERR_FORMAT;
    const uint32_t digit = (uint32_t)(text.data[i] - '0');
    if (number > ((uint32_t)INT32_MAX - digit) / 10u) return H2_PAL_ERR_FORMAT;
    number = number * 10u + digit;
  }
  *out = (int32_t)number;
  return H2_PAL_OK;
}

static h2_pal_result_t read_string(const h2_pal_json_api_t* json,
                                   const h2_pal_json_value_t* object,
                                   const char* key, char* out,
                                   size_t capacity) {
  h2_pal_json_value_t* value = NULL;
  h2_pal_json_string_view_t text = {0};
  h2_pal_result_t rc = object_get(json, object, key, &value);
  if (rc == H2_PAL_OK) rc = h2_pal_json_value_get_string(json, value, &text);
  if (rc != H2_PAL_OK) return response_error(rc);
  if (text.len == 0u || text.len >= capacity ||
      memchr(text.data, '\0', text.len) != NULL)
    return H2_PAL_ERR_FORMAT;
  memcpy(out, text.data, text.len);
  out[text.len] = '\0';
  return H2_PAL_OK;
}

static h2_pal_result_t exchange(const h2_lingxi_config_t* config,
                                size_t base_len, const char* path, builder_t* b,
                                h2_lingxi_result_t* result,
                                h2_lingxi_device_info_t* info) {
  h2_pal_result_t rc = b->result;
  h2_pal_json_buffer_t encoded = {0};
  h2_pal_json_document_t* response_doc = NULL;
  h2_pal_http_response_t response = {0};
  uint8_t* body = NULL;
  char url[H2_LINGXI_BASE_URL_CAPACITY + 64u];
  const size_t path_len = strlen(path);
  if (base_len + path_len >= sizeof(url)) rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_serialize(b->json, b->document, &encoded);
  if (rc == H2_PAL_OK) {
    body = h2_pal_mem_alloc(config->allocator,
                            config->json_limits.max_document_bytes);
    if (body == NULL) rc = H2_PAL_ERR_NO_MEMORY;
  }
  if (rc != H2_PAL_OK) goto done;
  memcpy(url, config->base_url, base_len);
  memcpy(url + base_len, path, path_len + 1u);
  h2_pal_http_header_t headers[] = {
      {{"Content-Type", 12u}, {"application/json", 16u}},
      {{"X-AI-IP", 7u},
       {config->source_ip,
        config->source_ip != NULL ? strlen(config->source_ip) : 0u}},
  };
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_POST,
      .url = {url, base_len + path_len},
      .headers = headers,
      .header_count = config->source_ip != NULL ? 2u : 1u,
      .body = encoded.data,
      .body_len = encoded.len,
      .response_header_cb = reject_location,
      .interface_name = config->interface_name,
      .timeout_ms = (int)config->timeout_ms,
      .retry_count = 0,
      .response_buf = body,
      .response_buf_cap = config->json_limits.max_document_bytes,
      .cancel_cb = config->cancel,
      .cancel_user = config->cancel_user,
      .allocator = config->allocator,
  };
  rc = h2_pal_http_do(config->http, &request, &response);
  result->http_status = response.status_code;
  if (rc != H2_PAL_OK) goto done;
  if (response.status_code < 200 || response.status_code >= 300) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto done;
  }
  if (response.body_len > config->json_limits.max_document_bytes ||
      response.body == NULL || response.body != body) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  rc =
      h2_pal_json_document_parse(config->json, response.body, response.body_len,
                                 &config->json_limits, &response_doc);
  h2_pal_json_value_t* root = NULL;
  h2_pal_json_value_t* value = NULL;
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_root(config->json, response_doc, &root);
  if (rc == H2_PAL_OK) rc = object_get(config->json, root, "code", &value);
  if (rc == H2_PAL_OK)
    rc = read_code(config->json, value, &result->platform_code);
  if (rc == H2_PAL_OK) rc = object_get(config->json, root, "success", &value);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_value_get_boolean(config->json, value,
                                       &result->platform_success);
  if (rc != H2_PAL_OK) {
    rc = response_error(rc);
    goto done;
  }
  if (!result->platform_success || result->platform_code != 200) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto done;
  }
  if (info != NULL) {
    rc = object_get(config->json, root, "data", &value);
    if (rc == H2_PAL_OK)
      rc = read_string(config->json, value, "productId", info->product_id,
                       sizeof(info->product_id));
    if (rc == H2_PAL_OK)
      rc = read_string(config->json, value, "deviceNo", info->device_no,
                       sizeof(info->device_no));
    if (rc == H2_PAL_OK)
      rc = read_string(config->json, value, "deviceId", info->device_id,
                       sizeof(info->device_id));
    if (rc == H2_PAL_OK)
      rc = read_string(config->json, value, "deviceSecret", info->device_secret,
                       sizeof(info->device_secret));
    rc = response_error(rc);
  }
done: {
  const h2_pal_result_t released =
      h2_pal_json_document_destroy(config->json, &response_doc);
  if (rc == H2_PAL_OK) rc = released;
}
  h2_pal_http_response_free(config->http, &response);
  if (body != NULL) wipe(body, config->json_limits.max_document_bytes);
  h2_pal_mem_free(config->allocator, body);
  if (encoded.data != NULL) wipe(encoded.data, encoded.len);
  {
    const h2_pal_result_t released =
        h2_pal_json_buffer_release(b->json, &encoded);
    if (rc == H2_PAL_OK) rc = released;
  }
  {
    const h2_pal_result_t released =
        h2_pal_json_document_destroy(b->json, &b->document);
    if (rc == H2_PAL_OK) rc = released;
  }
  return rc;
}

h2_pal_result_t h2_lingxi_get_device_info(const h2_lingxi_config_t* config,
                                          const h2_lingxi_product_t* product,
                                          const h2_lingxi_identity_t* identity,
                                          h2_lingxi_device_info_t* out_info,
                                          h2_lingxi_result_t* out_result) {
  h2_lingxi_device_info_clear(out_info);
  if (out_result != NULL) memset(out_result, 0, sizeof(*out_result));
  size_t base_len = 0u;
  if (out_info == NULL || out_result == NULL ||
      !config_valid(config, &base_len) || !product_valid(product) ||
      identity == NULL ||
      !text_valid(identity->number, H2_LINGXI_DEVICE_NO_CAPACITY) ||
      identity->type < H2_LINGXI_DEVICE_NO_SN ||
      identity->type > H2_LINGXI_DEVICE_NO_CMEI)
    return H2_PAL_ERR_INVALID_ARG;
  const char* const types[] = {"SN", "IMEI", "CMEI"};
  builder_t builder;
  builder_create(&builder, config);
  add_product(&builder, product);
  add_string(&builder, builder.root, "deviceNoType", types[identity->type - 1]);
  add_string(&builder, builder.root, "deviceNo", identity->number);
  h2_pal_result_t rc =
      exchange(config, base_len, "/v2/customer/device/secret/info", &builder,
               out_result, out_info);
  if (rc == H2_PAL_OK && (strcmp(out_info->product_id, product->id) != 0 ||
                          strcmp(out_info->device_no, identity->number) != 0))
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK) h2_lingxi_device_info_clear(out_info);
  return rc;
}

static bool heartbeat_valid(const h2_lingxi_heartbeat_t* heartbeat) {
  if (heartbeat == NULL || heartbeat->inner_ips == NULL ||
      heartbeat->inner_ip_count == 0u ||
      heartbeat->inner_ip_count > H2_LINGXI_MAX_INNER_IPS ||
      !text_valid(heartbeat->net_speed, H2_LINGXI_TEXT_CAPACITY) ||
      !text_valid(heartbeat->net_type, H2_LINGXI_TEXT_CAPACITY) ||
      !text_valid(heartbeat->platform, H2_LINGXI_TEXT_CAPACITY) ||
      !text_valid(heartbeat->sdk_version, H2_LINGXI_TEXT_CAPACITY) ||
      strncmp(heartbeat->sdk_version, "ai_", 3u) != 0 ||
      !text_valid(heartbeat->firmware_version, H2_LINGXI_TEXT_CAPACITY))
    return false;
  for (size_t i = 0u; i < heartbeat->inner_ip_count; ++i)
    if (!text_valid(heartbeat->inner_ips[i], H2_LINGXI_IP_CAPACITY))
      return false;
  const char* optional[] = {heartbeat->imei, heartbeat->cmei, heartbeat->mac};
  for (size_t i = 0u; i < sizeof(optional) / sizeof(optional[0]); ++i)
    if (optional[i] != NULL &&
        bounded_length(optional[i], H2_LINGXI_TEXT_CAPACITY) ==
            H2_LINGXI_TEXT_CAPACITY)
      return false;
  return true;
}

h2_pal_result_t h2_lingxi_report_heartbeat(
    const h2_lingxi_config_t* config, const h2_lingxi_product_t* product,
    const h2_lingxi_device_info_t* device,
    const h2_lingxi_heartbeat_t* heartbeat, h2_lingxi_result_t* out_result) {
  if (out_result != NULL) memset(out_result, 0, sizeof(*out_result));
  size_t base_len = 0u;
  if (out_result == NULL || !config_valid(config, &base_len) ||
      !product_valid(product) || device == NULL ||
      !text_valid(device->product_id, sizeof(device->product_id)) ||
      strcmp(device->product_id, product->id) != 0 ||
      !text_valid(device->device_id, sizeof(device->device_id)) ||
      !text_valid(device->device_secret, sizeof(device->device_secret)) ||
      !heartbeat_valid(heartbeat))
    return H2_PAL_ERR_INVALID_ARG;
  builder_t b;
  builder_create(&b, config);
  add_product(&b, product);
  add_string(&b, b.root, "deviceId", device->device_id);
  add_string(&b, b.root, "deviceSecret", device->device_secret);
  h2_pal_json_value_t* params = NULL;
  h2_pal_json_value_t* ips = NULL;
  if (b.result == H2_PAL_OK)
    b.result = h2_pal_json_value_create_object(b.json, b.document, &params);
  if (b.result == H2_PAL_OK)
    b.result = h2_pal_json_object_set(b.json, b.root, "params", 6u, params);
  if (b.result == H2_PAL_OK)
    b.result = h2_pal_json_value_create_array(b.json, b.document, &ips);
  if (b.result == H2_PAL_OK)
    b.result = h2_pal_json_object_set(b.json, params, "innerIp", 7u, ips);
  for (size_t i = 0u; b.result == H2_PAL_OK && i < heartbeat->inner_ip_count;
       ++i) {
    h2_pal_json_value_t* value = NULL;
    b.result = h2_pal_json_value_create_string(
        b.json, b.document, heartbeat->inner_ips[i],
        strlen(heartbeat->inner_ips[i]), &value);
    if (b.result == H2_PAL_OK)
      b.result = h2_pal_json_array_append(b.json, ips, value);
  }
  add_string(&b, params, "netSpeed", heartbeat->net_speed);
  add_string(&b, params, "netType", heartbeat->net_type);
  add_string(&b, params, "platform", heartbeat->platform);
  add_string(&b, params, "sdkVersion", heartbeat->sdk_version);
  add_string(&b, params, "firmwareVersion", heartbeat->firmware_version);
  add_string(&b, params, "imei",
             heartbeat->imei != NULL ? heartbeat->imei : "");
  add_string(&b, params, "cmei",
             heartbeat->cmei != NULL ? heartbeat->cmei : "");
  add_string(&b, params, "mac", heartbeat->mac != NULL ? heartbeat->mac : "");
  return exchange(config, base_len, "/v2/customer/device/report", &b,
                  out_result, NULL);
}
