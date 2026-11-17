#include "h2_alipay_transit_internal.h"

#include "h2_alipay_client_internal.h"
#include "h2_encoding.h"
#include "h2_javascript.h"
#include "yyjson.h"
#include <stdio.h>
#include <string.h>

static int native_result(const h2_pal_mem_api_t* memory, const char* input,
                         size_t length, char** result, size_t* size) {
  *result = h2_pal_mem_alloc(memory, length + 1u);
  if (*result == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(*result, input, length);
  (*result)[length] = '\0';
  *size = length;
  return H2_PAL_OK;
}

static int native_method(void* user, const char* method, const char* parameters,
                         const h2_pal_mem_api_t* memory, char** result,
                         size_t* result_length) {
  const h2_alipay_client_config_t* config = user;
  *result = NULL;
  *result_length = 0u;
  if (strcmp(method, "utcTime") == 0) {
    uint64_t milliseconds = 0u;
    int rc = h2_pal_time_get_wall_ms(config->time, &milliseconds);
    if (rc != H2_PAL_OK) return rc;
    uint32_t raw = (uint32_t)(milliseconds / 1000u);
    int64_t seconds = raw <= INT32_MAX ? raw : (int64_t)raw - 4294967296;
    char value[32];
    int size = snprintf(value, sizeof(value), "%lld123", (long long)seconds);
    if (size < 0 || (size_t)size >= sizeof(value)) return H2_PAL_ERR_FORMAT;
    return native_result(memory, value, (size_t)size, result, result_length);
  }
  bool md5 = strcmp(method, "md5") == 0;
  bool sha1 = strcmp(method, "sha1") == 0;
  bool sha256 = strcmp(method, "sha256") == 0;
  bool sm3 = strcmp(method, "sm3") == 0;
  bool base64 = strcmp(method, "base64") == 0;
  bool hmac = strcmp(method, "hmac") == 0;
  bool aes = strcmp(method, "aes") == 0;
  bool des = strcmp(method, "des") == 0;
  bool retail = strcmp(method, "pboc_3des_mac") == 0;
  if (!md5 && !sha1 && !sha256 && !sm3 && !base64 && !hmac && !aes && !des &&
      !retail)
    return H2_PAL_ERR_UNSUPPORTED;
  yyjson_alc allocator = {h2_alipay_client_json_alloc,
                          h2_alipay_client_json_realloc,
                          h2_alipay_client_json_free, (void*)memory};
  yyjson_read_err error;
  yyjson_doc* document =
      yyjson_read_opts((char*)parameters, strlen(parameters),
                       YYJSON_READ_STOP_WHEN_DONE, &allocator, &error);
  if (document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  const char* encoded = yyjson_get_str(
      h2_alipay_client_json_member(yyjson_doc_get_root(document), "hexData"));
  struct native_crypto_work {
    uint8_t input[1024], key[256], result[1536];
    char text[3073];
  };
  struct native_crypto_work* work = h2_pal_mem_alloc(memory, sizeof(*work));
  if (work == NULL) {
    yyjson_doc_free(document);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(work, 0, sizeof(*work));
  size_t input_length = 0u, key_length = 0u, size = 0u, binary_length = 0u;
  int rc = encoded != NULL && strlen(encoded) <= 2048u ? H2_PAL_OK
                                                       : H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && !md5)
    rc =
        h2_encoding_decode(&h2_encoding_hex, encoded, strlen(encoded) / 2u * 2u,
                           work->input, sizeof(work->input), &input_length);
  if (rc == H2_PAL_OK && (hmac || aes || des || retail)) {
    const char* key_text = yyjson_get_str(h2_alipay_client_json_member(
        yyjson_doc_get_root(document), "privateKey"));
    rc = key_text != NULL && strlen(key_text) <= sizeof(work->key) * 2u
             ? h2_encoding_decode(&h2_encoding_hex, key_text,
                                  strlen(key_text) / 2u * 2u, work->key,
                                  sizeof(work->key), &key_length)
             : H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK) {
    if (md5) {
      binary_length = 16u;
      rc = h2_crypto_md5(config->crypto, (const uint8_t*)encoded,
                         strlen(encoded), work->result);
    } else if (sha1) {
      binary_length = 20u;
      rc = h2_crypto_sha1(config->crypto, work->input, input_length,
                          work->result);
    } else if (sha256) {
      binary_length = 32u;
      rc = h2_crypto_sha256(work->input, input_length, work->result);
    } else if (sm3) {
      binary_length = 32u;
      rc = h2_crypto_sm3(work->input, input_length, work->result);
    } else if (base64) {
      rc = h2_encoding_encode(&h2_encoding_base64_std, work->input,
                              input_length, (char*)work->result,
                              sizeof(work->result), &binary_length);
    } else if (hmac) {
      binary_length = 16u;
      rc = h2_crypto_hmac_md5(config->crypto, work->key, key_length,
                              work->input, input_length, work->result);
    } else if (retail) {
      binary_length = 4u;
      rc = key_length == 16u ? h2_crypto_iso9797_alg3_mac(
                                   config->crypto, work->key, work->input,
                                   input_length, work->result)
                             : H2_PAL_ERR_INVALID_ARG;
    } else {
      uint8_t iv[16] = {0};
      h2_pal_crypto_buf_t encrypted = {work->result, 0u, sizeof(work->result)};
      rc = aes ? h2_crypto_aes_cbc(config->crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT,
                                   work->key, key_length, iv, work->input,
                                   input_length, &encrypted)
               : h2_crypto_des_cbc(config->crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT,
                                   work->key, key_length, iv, work->input,
                                   input_length, &encrypted);
      binary_length = encrypted.len;
    }
  }
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_hex, work->result, binary_length,
                            work->text, sizeof(work->text) - 1u, &size);
  if (rc == H2_PAL_OK)
    rc = native_result(memory, work->text, size, result, result_length);
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(memory, work);
  yyjson_doc_free(document);
  return rc;
}

int h2_alipay_transit_script_run(const h2_alipay_client_config_t* config,
                                 const char* script, yyjson_val* render,
                                 uint8_t* output, size_t* length) {
  const char* data =
      yyjson_get_str(h2_alipay_client_json_member(render, "offlineCert"));
  const char* private_key =
      yyjson_get_str(h2_alipay_client_json_member(render, "certPrivateKey"));
  if (script == NULL || config == NULL || output == NULL || length == NULL ||
      data == NULL)
    return 43;
  yyjson_alc allocator = {h2_alipay_client_json_alloc,
                          h2_alipay_client_json_realloc,
                          h2_alipay_client_json_free, (void*)config->mem};
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  yyjson_mut_val* parameters =
      document != NULL ? yyjson_mut_obj(document) : NULL;
  int result = 29;
  char* encoded = NULL;
  char* source = NULL;
  char* value = NULL;
  if (parameters == NULL ||
      !yyjson_mut_obj_add_str(document, parameters, "hexData", data) ||
      !yyjson_mut_obj_add_str(document, parameters, "privateKey",
                              private_key != NULL ? private_key : "") ||
      !yyjson_mut_obj_add_str(document, parameters, "qrcode", ""))
    goto done;
  yyjson_mut_doc_set_root(document, parameters);
  size_t parameter_length = 0u;
  encoded =
      yyjson_mut_write_opts(document, 0u, &allocator, &parameter_length, NULL);
  if (encoded == NULL) goto done;
  static const char prefix[] =
      "(function(){var console={log:function(){}};"
      "function callNative(method,param){"
      "return __h2_native(method,JSON.stringify(param||{}));}";
  static const char middle[] = ";var param=";
  static const char suffix[] =
      ";var result=getCode(param);return result?result.toString():null;})()";
  size_t script_length = strlen(script);
  if (script_length > 60000u || parameter_length > 8192u) {
    result = 43;
    goto done;
  }
  size_t source_length = sizeof(prefix) - 1u + script_length + sizeof(middle) -
                         1u + parameter_length + sizeof(suffix) - 1u;
  source = h2_pal_mem_alloc(config->mem, source_length + 1u);
  value = h2_pal_mem_alloc(config->mem, 1025u);
  if (source == NULL || value == NULL) goto done;
  size_t offset = 0u;
  memcpy(source + offset, prefix, sizeof(prefix) - 1u);
  offset += sizeof(prefix) - 1u;
  memcpy(source + offset, script, script_length);
  offset += script_length;
  memcpy(source + offset, middle, sizeof(middle) - 1u);
  offset += sizeof(middle) - 1u;
  memcpy(source + offset, encoded, parameter_length);
  offset += parameter_length;
  memcpy(source + offset, suffix, sizeof(suffix));
  h2_javascript_config_t engine = {.memory = config->mem,
                                   .time = config->time,
                                   .native = native_method,
                                   .user = (void*)config,
                                   .stack_limit = config->script_stack_limit};
  size_t size = 0u;
  int rc = h2_javascript_evaluate(&engine, source, source_length,
                                  (uint8_t*)value, 1024u, &size);
  if (rc != H2_PAL_OK) {
    result = rc == H2_PAL_ERR_NO_MEMORY ? 29 : 44;
    goto done;
  }
  value[size] = '\0';
  size_t reported = size / 2u;
  bool odd = size % 2u != 0u;
  if ((size + 1u) / 2u > *length) {
    result = 15;
    goto done;
  }
  if (odd) value[size++] = '0';
  size_t written = 0u;
  rc = h2_encoding_decode(&h2_encoding_hex, value, size, output, *length,
                          &written);
  if (rc == H2_PAL_OK) {
    if (odd && written != 0u) output[written - 1u] |= 0xd0u;
    *length = reported;
    result = 0;
  } else {
    result = 44;
  }
done:
  if (source != NULL) {
    h2_alipay_client_wipe(source, source_length + 1u);
    h2_pal_mem_free(config->mem, source);
  }
  if (value != NULL) {
    h2_alipay_client_wipe(value, 1025u);
    h2_pal_mem_free(config->mem, value);
  }
  if (encoded != NULL) allocator.free(allocator.ctx, encoded);
  yyjson_mut_doc_free(document);
  return result;
}
