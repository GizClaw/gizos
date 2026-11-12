#include "h2_alipay_protocol.h"

#include "h2_encoding.h"
#include "h2_sha256.h"
#include "h2_sm3.h"
#include "yyjson.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

h2_pal_result_t h2_alipay_protocol_timestamp(uint64_t utc_ms, char out[24]) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  uint64_t seconds = utc_ms / 1000u;
  if (seconds == 0u || seconds > INT32_MAX) return H2_PAL_ERR_INVALID_STATE;
  (void)snprintf(out, 24u, "%" PRIu64, seconds * 1000u);
  return H2_PAL_OK;
}

static bool valid_text(const char* value, size_t maximum) {
  if (value == NULL || *value == '\0') return false;
  for (size_t index = 0u; index <= maximum; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte == 0u) return true;
    if (index == maximum || byte < 0x20u || byte > 0x7eu) return false;
  }
  return false;
}

static bool valid_serial(const char* value, size_t maximum) {
  if (!valid_text(value, maximum)) return false;
  for (size_t index = 0u; value[index] != '\0'; ++index) {
    char byte = value[index];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_'))
      return false;
  }
  return true;
}

static bool valid_identity(const h2_alipay_protocol_identity_t* identity) {
  if (identity == NULL || !valid_serial(identity->sn, 32u) ||
      !valid_serial(identity->imei, 48u) || !valid_text(identity->model, 23u) ||
      strchr(identity->model, '#') != NULL || !valid_text(identity->mac, 17u) ||
      strlen(identity->mac) != 17u)
    return false;
  for (size_t index = 0u; index < 17u; ++index) {
    char byte = identity->mac[index];
    if (index % 3u == 2u) {
      if (byte != ':') return false;
    } else if (!((byte >= '0' && byte <= '9') ||
                 (byte >= 'A' && byte <= 'F'))) {
      return false;
    }
  }
  return true;
}

static void hash_text(h2_sha256_t* context, const char* text) {
  h2_sha256_update(context, (const uint8_t*)text, strlen(text));
}

h2_pal_result_t h2_alipay_protocol_signing_key(
    const h2_alipay_protocol_identity_t* identity, const char* device_name,
    uint8_t out[32]) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, 32u);
  if (!valid_identity(identity) || !valid_text(device_name, 131u))
    return H2_PAL_ERR_INVALID_ARG;
  h2_sha256_t context;
  h2_sha256_init(&context);
  hash_text(&context, "ALIPAY_BRAND_NAME_");
  hash_text(&context, identity->model);
  hash_text(&context, "##");
  hash_text(&context, identity->sn);
  hash_text(&context, "##");
  hash_text(&context, device_name);
  h2_sha256_finish(&context, out);
  uint8_t device_hash[32];
  h2_sha256_init(&context);
  hash_text(&context, identity->imei);
  hash_text(&context, "##");
  hash_text(&context, identity->model);
  hash_text(&context, "##");
  hash_text(&context, identity->sn);
  h2_sha256_finish(&context, device_hash);
  for (size_t index = 0u; index < 32u; ++index)
    out[index] ^= device_hash[index];
  volatile uint8_t* clear = device_hash;
  for (size_t index = 0u; index < sizeof(device_hash); ++index)
    clear[index] = 0u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_protocol_canonical(const char* operation,
                                             const char* timestamp, bool binary,
                                             const uint8_t* body,
                                             size_t body_length, char* out,
                                             size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (!valid_text(operation, 127u) || !valid_text(timestamp, 64u) ||
      strchr(operation, '&') != NULL || strchr(timestamp, '&') != NULL ||
      (body == NULL && body_length != 0u) || (out == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  size_t encoded = 0u;
  if (body_length != 0u) {
    if (body_length > (SIZE_MAX - 2u) / 4u * 3u) return H2_PAL_ERR_NO_SPACE;
    encoded = (body_length + 2u) / 3u * 4u;
  } else {
    encoded = binary ? 0u : 2u;
  }
  static const char first[] = "Operation-Type=";
  static const char middle[] = "&Request-Data=";
  static const char last[] = "&Ts=";
  size_t prefix = sizeof(first) - 1u + strlen(operation) + sizeof(middle) - 1u;
  size_t suffix = sizeof(last) - 1u + strlen(timestamp);
  if (encoded > SIZE_MAX - prefix - suffix - 1u) return H2_PAL_ERR_NO_SPACE;
  *length = prefix + encoded + suffix;
  if (capacity <= *length) return H2_PAL_ERR_NO_SPACE;
  size_t used = 0u;
  memcpy(out + used, first, sizeof(first) - 1u);
  used += sizeof(first) - 1u;
  memcpy(out + used, operation, strlen(operation));
  used += strlen(operation);
  memcpy(out + used, middle, sizeof(middle) - 1u);
  used += sizeof(middle) - 1u;
  if (body_length != 0u) {
    size_t written = 0u;
    if (h2_encoding_encode(&h2_encoding_base64_std, body, body_length,
                           out + used, encoded, &written) != H2_ENCODING_OK)
      return H2_PAL_ERR_FORMAT;
  } else if (!binary) {
    memcpy(out + used, "[]", 2u);
  }
  used += encoded;
  memcpy(out + used, last, sizeof(last) - 1u);
  used += sizeof(last) - 1u;
  memcpy(out + used, timestamp, strlen(timestamp) + 1u);
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_protocol_signature(const uint8_t key[32],
                                             const uint8_t* data, size_t length,
                                             char out[65]) {
  if (key == NULL || out == NULL || (data == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  static const char alphabet[] = "0123456789ABCDEF";
  uint8_t digest[32];
  h2_sm3_hmac(key, 32u, data, length, digest);
  for (size_t index = 0u; index < sizeof(digest); ++index) {
    out[index * 2u] = alphabet[digest[index] >> 4u];
    out[index * 2u + 1u] = alphabet[digest[index] & 15u];
  }
  out[64] = '\0';
  return H2_PAL_OK;
}

static size_t varint_size(uint64_t value) {
  size_t size = 1u;
  while (value >= 128u) {
    value >>= 7u;
    ++size;
  }
  return size;
}

static size_t write_varint(uint8_t* out, uint64_t value) {
  size_t used = 0u;
  do {
    out[used++] = (uint8_t)((value & 127u) | (value >= 128u ? 128u : 0u));
    value >>= 7u;
  } while (value != 0u);
  return used;
}

static size_t write_field(uint8_t* out, uint8_t tag, const char* value) {
  size_t length = strlen(value);
  out[0] = tag;
  size_t used = 1u + write_varint(out + 1u, length);
  memcpy(out + used, value, length);
  return used + length;
}

h2_pal_result_t h2_alipay_protocol_registration(
    const h2_alipay_protocol_identity_t* identity, uint8_t* out,
    size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (!valid_identity(identity) || (out == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  char vendor[64];
  (void)snprintf(vendor, sizeof(vendor), "ALIPAY_BRAND_NAME_%s",
                 identity->model);
  const char* fields[] = {
      "AlipayInside_IoTPay_NET",
      identity->sn,
      vendor,
      identity->imei,
      identity->mac,
      identity->model,
      identity->imei,
      identity->sn,
  };
  size_t required = 4u;
  for (size_t index = 0u; index < sizeof(fields) / sizeof(*fields); ++index) {
    size_t size = strlen(fields[index]);
    required += 1u + varint_size(size) + size;
  }
  *length = required;
  if (capacity < required) return H2_PAL_ERR_NO_SPACE;
  size_t used = 0u;
  for (size_t index = 0u; index < sizeof(fields) / sizeof(*fields); ++index)
    used +=
        write_field(out + used, index < 3u ? (uint8_t)(10u + index * 8u) : 34u,
                    fields[index]);
  out[used++] = 40u;
  out[used++] = 6u;
  out[used++] = 48u;
  out[used] = 1u;
  return H2_PAL_OK;
}

static bool read_varint(const uint8_t* data, size_t length, size_t* cursor,
                        uint64_t* value) {
  *value = 0u;
  for (unsigned index = 0u; index < 10u && *cursor < length; ++index) {
    uint8_t byte = data[(*cursor)++];
    if (index == 9u && byte > 1u) return false;
    *value |= (uint64_t)(byte & 127u) << (index * 7u);
    if ((byte & 128u) == 0u) return true;
  }
  return false;
}

h2_pal_result_t h2_alipay_protocol_device_name(const uint8_t* data,
                                               size_t length, char* out,
                                               size_t capacity) {
  if (data == NULL || out == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  size_t cursor = 0u, name_offset = 0u, name_length = 0u;
  bool found = false;
  while (cursor < length) {
    uint64_t tag = 0u, size = 0u;
    if (!read_varint(data, length, &cursor, &tag) || tag < 8u ||
        tag >> 3u > 0x1fffffffu || (tag >> 3u == 1u && (tag & 7u) != 2u))
      return H2_PAL_ERR_FORMAT;
    unsigned wire = (unsigned)(tag & 7u);
    if (wire == 0u) {
      if (!read_varint(data, length, &cursor, &size)) return H2_PAL_ERR_FORMAT;
      continue;
    }
    if (wire == 2u) {
      if (!read_varint(data, length, &cursor, &size)) return H2_PAL_ERR_FORMAT;
    } else if (wire == 1u) {
      size = 8u;
    } else if (wire == 5u) {
      size = 4u;
    } else {
      return H2_PAL_ERR_FORMAT;
    }
    if (size > length - cursor) return H2_PAL_ERR_FORMAT;
    if (tag >> 3u == 1u) {
      if (found || wire != 2u || size == 0u || size > 131u)
        return H2_PAL_ERR_FORMAT;
      found = true;
      name_offset = cursor;
      name_length = (size_t)size;
      for (size_t index = 0u; index < name_length; ++index)
        if (data[cursor + index] < 0x21u || data[cursor + index] > 0x7eu)
          return H2_PAL_ERR_FORMAT;
    }
    cursor += (size_t)size;
  }
  if (!found) return H2_PAL_ERR_FORMAT;
  if (name_length >= capacity) return H2_PAL_ERR_NO_SPACE;
  memcpy(out, data + name_offset, name_length);
  out[name_length] = '\0';
  return H2_PAL_OK;
}

static void* json_alloc(void* user, size_t size) {
  return h2_pal_mem_alloc(user, size);
}

static void* json_realloc(void* user, void* previous, size_t old_size,
                          size_t size) {
  (void)old_size;
  return h2_pal_mem_realloc(user, previous, size);
}

static void json_free(void* user, void* data) { h2_pal_mem_free(user, data); }

static bool valid_company(const char* company) {
  if (!valid_text(company, 15u)) return false;
  for (size_t index = 0u; company[index] != '\0'; ++index)
    if (company[index] < 'A' || company[index] > 'Z') return false;
  return true;
}

static void model_and_version(const h2_alipay_protocol_identity_t* identity,
                              const char* company, uint32_t feature,
                              uint32_t timestamp, char model[64],
                              char version[64]) {
  (void)snprintf(model, 64u, "QZ.SB.LY.%s-%s", company, identity->model);
  (void)snprintf(version, 64u, "4.612.%" PRIu32 ".%" PRIu32, feature,
                 timestamp / 10000u);
}

h2_pal_result_t h2_alipay_protocol_prepare_bind(
    const h2_pal_mem_api_t* mem, const h2_alipay_protocol_identity_t* identity,
    const char* company, const char* biztid, uint32_t feature,
    uint32_t compile_timestamp_s, const uint8_t public_key[64], char** out,
    size_t* length) {
  if (out == NULL || length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  *length = 0u;
  if (feature != 0x4001u) return H2_PAL_ERR_UNSUPPORTED;
  if (mem == NULL || !valid_identity(identity) || !valid_company(company) ||
      !valid_text(biztid, 32u) || public_key == NULL ||
      compile_timestamp_s == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  char model[64], version[64], factor[129];
  model_and_version(identity, company, feature, compile_timestamp_s, model,
                    version);
  static const char hex[] = "0123456789ABCDEF";
  for (size_t index = 0u; index < 64u; ++index) {
    factor[index * 2u] = hex[public_key[index] >> 4u];
    factor[index * 2u + 1u] = hex[public_key[index] & 15u];
  }
  factor[128] = '\0';
  yyjson_alc allocator = {json_alloc, json_realloc, json_free, (void*)mem};
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* root = yyjson_mut_arr(document);
  yyjson_mut_val* request = yyjson_mut_obj(document);
  yyjson_mut_val* device = yyjson_mut_obj(document);
  yyjson_mut_val* extensions = yyjson_mut_obj(document);
  yyjson_mut_val* lengths = yyjson_mut_arr(document);
  bool ok =
      root != NULL && request != NULL && device != NULL && extensions != NULL &&
      lengths != NULL &&
      yyjson_mut_obj_add_str(document, device, "ecdhFactor", factor) &&
      yyjson_mut_obj_add_str(document, device, "imei", identity->imei) &&
      yyjson_mut_obj_add_str(document, device, "imsi", identity->imei) &&
      yyjson_mut_obj_add_str(document, device, "clientKey", identity->imei) &&
      yyjson_mut_obj_add_str(document, device, "appProductVersion",
                             identity->imei) &&
      yyjson_mut_obj_add_str(document, device, "bluetoothMac", identity->mac) &&
      yyjson_mut_obj_add_str(document, device, "deviceModel", model) &&
      yyjson_mut_obj_add_str(document, device, "tidSource", "changeCode") &&
      yyjson_mut_obj_add_str(document, device, "insideModel", "POCKET_MONEY") &&
      yyjson_mut_obj_add_str(document, device, "appPackageName",
                             "alipay.iotpay.changecode.network") &&
      yyjson_mut_obj_add_str(document, device, "appProductId",
                             "ALIPAY_INSIDE_CHANGECODEWATCH") &&
      yyjson_mut_obj_add_str(document, device, "appSource",
                             "INSIDE_CHANGECODEWATCH") &&
      yyjson_mut_obj_add_str(document, device, "otpAlgorithm", "0") &&
      yyjson_mut_obj_add_str(document, device, "sdkVersion", version) &&
      yyjson_mut_arr_add_str(document, lengths, "CODE_19") &&
      yyjson_mut_arr_add_str(document, lengths, "CODE_18") &&
      yyjson_mut_obj_add_val(document, device, "supportedCodeLength",
                             lengths) &&
      yyjson_mut_obj_add_str(document, extensions, "sourceDeviceId",
                             identity->sn) &&
      yyjson_mut_obj_add_str(document, extensions, "brand", company) &&
      yyjson_mut_obj_add_str(document, extensions, "solutionVendor", biztid) &&
      yyjson_mut_obj_add_val(document, device, "extInfos", extensions) &&
      yyjson_mut_obj_add_str(document, request, "operationMode", "NETWORK") &&
      yyjson_mut_obj_add_val(document, request, "deviceInfo", device) &&
      yyjson_mut_arr_append(root, request);
  if (ok) {
    yyjson_mut_doc_set_root(document, root);
    *out = yyjson_mut_write_opts(document, 0u, &allocator, length, NULL);
  }
  yyjson_mut_doc_free(document);
  return *out != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

h2_pal_result_t h2_alipay_protocol_binding_code(
    const h2_alipay_protocol_identity_t* identity, const char* company,
    const char* token, uint32_t feature, uint32_t compile_timestamp_s,
    char* out, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (feature != 0x4001u) return H2_PAL_ERR_UNSUPPORTED;
  if (!valid_identity(identity) || !valid_company(company) ||
      !valid_text(token, 128u) || strchr(token, '#') != NULL ||
      compile_timestamp_s == 0u || (out == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  char model[64], version[64];
  model_and_version(identity, company, feature, compile_timestamp_s, model,
                    version);
  int required =
      snprintf(NULL, 0, "ALIPAY_QZWT#0#%s#%s#%s", token, model, version);
  if (required < 0) return H2_PAL_ERR_FORMAT;
  *length = (size_t)required;
  if (capacity <= *length) return H2_PAL_ERR_NO_SPACE;
  (void)snprintf(out, capacity, "ALIPAY_QZWT#0#%s#%s#%s", token, model,
                 version);
  return H2_PAL_OK;
}
