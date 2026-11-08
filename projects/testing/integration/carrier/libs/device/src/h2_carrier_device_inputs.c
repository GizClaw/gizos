#include "h2_carrier_device_inputs.h"

#include <string.h>

void h2_carrier_device_inputs_clear(h2_carrier_device_inputs_t* inputs) {
  if (inputs == NULL) return;
  volatile unsigned char* p = (volatile unsigned char*)inputs;
  for (size_t i = 0u; i < sizeof(*inputs); ++i) p[i] = 0u;
}

static h2_pal_result_t member(const h2_pal_json_api_t* json,
                              const h2_pal_json_value_t* root, const char* name,
                              h2_pal_json_value_t** out) {
  return h2_pal_json_object_get(json, root, name, strlen(name), out);
}

static h2_pal_result_t text(const h2_pal_json_api_t* json,
                            const h2_pal_json_value_t* object, const char* name,
                            char* out, size_t cap) {
  h2_pal_json_value_t* value = NULL;
  h2_pal_json_string_view_t string = {0};
  h2_pal_result_t rc = member(json, object, name, &value);
  if (rc == H2_PAL_OK) rc = h2_pal_json_value_get_string(json, value, &string);
  if (rc != H2_PAL_OK) return rc;
  if (string.len == 0u || string.len >= cap ||
      memchr(string.data, '\0', string.len))
    return H2_PAL_ERR_FORMAT;
  memcpy(out, string.data, string.len);
  out[string.len] = '\0';
  return H2_PAL_OK;
}

static h2_pal_result_t number(const h2_pal_json_api_t* json,
                              const h2_pal_json_value_t* object,
                              const char* name, uint64_t max, uint64_t* out) {
  h2_pal_json_value_t* value = NULL;
  double n = 0.0;
  h2_pal_result_t rc = member(json, object, name, &value);
  if (rc == H2_PAL_OK) rc = h2_pal_json_value_get_number(json, value, &n);
  if (rc != H2_PAL_OK) return rc;
  if (!(n >= 1.0 && n <= (double)max) || (double)(uint64_t)n != n)
    return H2_PAL_ERR_FORMAT;
  *out = (uint64_t)n;
  return H2_PAL_OK;
}

static h2_pal_result_t optional_text(const h2_pal_json_api_t* json,
                                     const h2_pal_json_value_t* object,
                                     const char* name, char* out,
                                     size_t capacity) {
  h2_pal_json_value_t* value = NULL;
  const h2_pal_result_t rc = member(json, object, name, &value);
  return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK
         : rc != H2_PAL_OK          ? rc
                                    : text(json, object, name, out, capacity);
}

h2_pal_result_t h2_carrier_device_inputs_parse(
    const h2_pal_json_api_t* json, const uint8_t* bytes, size_t length,
    h2_carrier_device_inputs_t* out) {
  h2_carrier_device_inputs_clear(out);
  if (json == NULL || bytes == NULL || out == NULL || length == 0u ||
      length > H2_CARRIER_DEVICE_CONFIG_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (json->vtable == NULL || json->vtable->document_parse == NULL ||
      json->vtable->document_destroy == NULL ||
      json->vtable->document_root == NULL || json->vtable->object_get == NULL ||
      json->vtable->value_get_string == NULL ||
      json->vtable->value_get_number == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_json_document_t* doc = NULL;
  h2_pal_json_value_t *root = NULL, *lingxi = NULL, *onenet = NULL;
  const h2_pal_json_limits_t limits = {H2_CARRIER_DEVICE_CONFIG_BYTES, 16u,
                                       256u};
  h2_pal_result_t rc =
      h2_pal_json_document_parse(json, bytes, length, &limits, &doc);
  if (rc == H2_PAL_OK) rc = h2_pal_json_document_root(json, doc, &root);
  if (rc == H2_PAL_OK)
    rc = number(json, root, "epoch_ms", UINT64_C(9007199254740991),
                &out->epoch_ms);
  if (rc == H2_PAL_OK) {
    rc = member(json, root, "lingxi", &lingxi);
    if (rc == H2_PAL_ERR_NOT_FOUND)
      rc = H2_PAL_OK;
    else if (rc == H2_PAL_OK)
      out->has_lingxi = true;
  }
  if (rc == H2_PAL_OK) {
    rc = member(json, root, "onenet", &onenet);
    if (rc == H2_PAL_ERR_NOT_FOUND)
      rc = H2_PAL_OK;
    else if (rc == H2_PAL_OK)
      out->has_onenet = true;
  }
  if (rc == H2_PAL_OK && !out->has_lingxi && !out->has_onenet)
    rc = H2_PAL_ERR_FORMAT;
#define READ(object, key, field) \
  if (rc == H2_PAL_OK)           \
  rc = text(json, object, key, out->field, sizeof(out->field))
  if (out->has_lingxi) {
    READ(lingxi, "base_url", lingxi_base);
    READ(lingxi, "product_id", lingxi_product);
    READ(lingxi, "product_key", lingxi_key);
    READ(lingxi, "device_no", device_no);
    char type[8] = {0};
    if (rc == H2_PAL_OK)
      rc = text(json, lingxi, "device_no_type", type, sizeof(type));
    if (rc == H2_PAL_OK) {
      if (strcmp(type, "SN") == 0)
        out->device_no_type = H2_LINGXI_DEVICE_NO_SN;
      else if (strcmp(type, "IMEI") == 0)
        out->device_no_type = H2_LINGXI_DEVICE_NO_IMEI;
      else if (strcmp(type, "CMEI") == 0)
        out->device_no_type = H2_LINGXI_DEVICE_NO_CMEI;
      else
        rc = H2_PAL_ERR_FORMAT;
    }
  }
  if (out->has_onenet) {
    READ(onenet, "host", onenet_host);
    if (rc == H2_PAL_OK)
      rc = optional_text(json, onenet, "ca_pem", out->onenet_ca_pem,
                         sizeof(out->onenet_ca_pem));
    if (rc == H2_PAL_OK)
      rc = optional_text(json, onenet, "tls_server_name",
                         out->onenet_tls_server_name,
                         sizeof(out->onenet_tls_server_name));
    if (rc == H2_PAL_OK && out->onenet_tls_server_name[0] != '\0' &&
        out->onenet_ca_pem[0] == '\0')
      rc = H2_PAL_ERR_FORMAT;
    READ(onenet, "product_id", onenet_product);
    READ(onenet, "device_name", onenet_device);
    if (rc == H2_PAL_OK) {
      h2_pal_json_value_t* token = NULL;
      rc = member(json, onenet, "access_token", &token);
      if (rc == H2_PAL_ERR_NOT_FOUND)
        rc = text(json, onenet, "device_key", out->onenet_device_key,
                  sizeof(out->onenet_device_key));
      else if (rc == H2_PAL_OK)
        rc = text(json, onenet, "access_token", out->onenet_token,
                  sizeof(out->onenet_token));
    }
    READ(onenet, "params_json", onenet_params);
    uint64_t port = 0u;
    if (rc == H2_PAL_OK) rc = number(json, onenet, "port", 65535u, &port);
    if (rc == H2_PAL_OK) out->onenet_port = (uint16_t)port;
  }
#undef READ
  const h2_pal_result_t released = h2_pal_json_document_destroy(json, &doc);
  if (rc == H2_PAL_OK) rc = released;
  if (rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_INVALID_STATE ||
      rc == H2_PAL_ERR_INVALID_ARG)
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK) h2_carrier_device_inputs_clear(out);
  return rc;
}
