#include "h2_alipay_client_internal.h"

#include "h2_encoding.h"
#include "yyjson.h"
#include <string.h>

static yyjson_alc provision_allocator(const h2_alipay_client_config_t* config) {
  const yyjson_alc allocator = {h2_alipay_client_json_alloc,
                                h2_alipay_client_json_realloc,
                                h2_alipay_client_json_free, (void*)config->mem};
  return allocator;
}

int h2_alipay_client_provision_receive(const h2_alipay_client_config_t* config,
                                       const char* input) {
  if (config == NULL || input == NULL) return 99;
  yyjson_alc allocator = provision_allocator(config);
  yyjson_read_err error;
  yyjson_doc* document =
      yyjson_read_opts((char*)input, strlen(input), YYJSON_READ_STOP_WHEN_DONE,
                       &allocator, &error);
  if (document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION ? 29 : 99;
  yyjson_val* root = yyjson_doc_get_root(document);
  bool valid =
      yyjson_is_true(h2_alipay_client_json_member(root, "success")) &&
      h2_alipay_client_json_member(root, "deviceCertificate") != NULL &&
      h2_alipay_client_json_member(root, "privateKey") != NULL;
  int result = 99;
  if (valid) {
    size_t length = 0u;
    char* encoded = yyjson_write_opts(document, 0u, &allocator, &length, NULL);
    if (encoded == NULL) {
      result = 29;
    } else {
      h2_pal_pref_namespace_t* namespace = NULL;
      int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                                H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
      if (rc == H2_PAL_OK && namespace == NULL) rc = H2_PAL_ERR_INVALID_STATE;
      if (rc == H2_PAL_OK) {
        rc = namespace->set_blob != NULL && namespace->commit != NULL
                 ? namespace->set_blob(namespace, "device_certificate", encoded,
                                       length)
                 : H2_PAL_ERR_UNSUPPORTED;
        if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
        rc = h2_alipay_client_finish_namespace(namespace, rc);
      }
      result = rc == H2_PAL_OK ? 0 : 5;
      allocator.free(allocator.ctx, encoded);
    }
  }
  yyjson_doc_free(document);
  return result;
}

static bool provision_fields(yyjson_mut_doc* document, yyjson_mut_val* object,
                             const h2_alipay_client_config_t* config,
                             const char* appid, bool activate) {
  return yyjson_mut_obj_add_str(document, object, "source", "WATCH_SDK") &&
         (appid == NULL ||
          yyjson_mut_obj_add_str(document, object, "companyCode", appid)) &&
         yyjson_mut_obj_add_str(document, object, "deviceModel",
                                config->identity.model) &&
         yyjson_mut_obj_add_uint(document, object, "productType",
                                 config->product_type) &&
         yyjson_mut_obj_add_str(document, object, "deviceSn",
                                config->identity.sn) &&
         yyjson_mut_obj_add_uint(document, object, "certType",
                                 activate ? 0u : 256u);
}

int h2_alipay_client_profile_certificate(
    const h2_alipay_client_config_t* config, const char* factor,
    yyjson_mut_doc* document, yyjson_mut_val* output) {
  if (config == NULL || factor == NULL || document == NULL || output == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  yyjson_alc allocator = provision_allocator(config);
  void* stored = NULL;
  size_t stored_length = 0u;
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = namespace->get_blob != NULL
           ? namespace->get_blob(namespace, config->mem, "device_certificate",
                                 &stored, &stored_length)
           : H2_PAL_ERR_UNSUPPORTED;
  rc = h2_alipay_client_finish_namespace(namespace, rc);
  yyjson_doc* factory = NULL;
  yyjson_mut_doc* business = NULL;
  char* message = NULL;
  h2_pal_p256_private_key_t private_key = {0};
  h2_pal_p256_signature_t signature = {0};
  uint8_t der[72] = {0};
  char encoded[97] = {0};
  if (rc != H2_PAL_OK) goto done;
  yyjson_read_err error;
  factory = yyjson_read_opts(stored, stored_length, YYJSON_READ_STOP_WHEN_DONE,
                             &allocator, &error);
  if (factory == NULL) {
    rc = error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
             ? H2_PAL_ERR_NO_MEMORY
             : H2_PAL_ERR_FORMAT;
    goto done;
  }
  yyjson_val* root = yyjson_doc_get_root(factory);
  yyjson_val* certificate =
      h2_alipay_client_json_member(root, "deviceCertificate");
  const char* private_text =
      yyjson_get_str(h2_alipay_client_json_member(root, "privateKey"));
  if (certificate == NULL || private_text == NULL ||
      strlen(private_text) != 64u) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  size_t private_length = 0u;
  rc =
      h2_encoding_decode(&h2_encoding_hex, private_text, 64u, private_key.bytes,
                         sizeof(private_key.bytes), &private_length);
  if (rc != H2_ENCODING_OK || private_length != sizeof(private_key.bytes)) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  yyjson_mut_val* copy = yyjson_val_mut_copy(document, certificate);
  if (copy == NULL ||
      !yyjson_mut_obj_add_val(document, output, "deviceCertificate", copy) ||
      !yyjson_mut_obj_add_str(document, output, "deviceVersion", "4.61")) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  business = yyjson_mut_doc_new(&allocator);
  yyjson_mut_val* payload = business != NULL ? yyjson_mut_obj(business) : NULL;
  if (payload == NULL ||
      !yyjson_mut_obj_add_str(business, payload, "ecdhFactor", factor)) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  yyjson_mut_doc_set_root(business, payload);
  size_t message_length = 0u;
  message =
      yyjson_mut_write_opts(business, 0u, &allocator, &message_length, NULL);
  if (message == NULL ||
      !yyjson_mut_obj_add_strcpy(document, output, "bizInformation", message) ||
      !yyjson_mut_obj_add_str(document, output, "bizSignAlgorithm",
                              "BASE64_OVER_ECDSA256")) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  rc = h2_pal_crypto_ecdsa_p256_sha256_sign(config->crypto, &private_key,
                                            (const uint8_t*)message,
                                            message_length, &signature);
  size_t der_length = 0u, encoded_length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_crypto_ecdsa_p256_der_encode(&signature, der, sizeof(der),
                                         &der_length);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_base64_std, der, der_length, encoded,
                            sizeof(encoded) - 1u, &encoded_length);
  if (rc == H2_PAL_OK) {
    encoded[encoded_length] = '\0';
    if (!yyjson_mut_obj_add_strcpy(document, output, "bizSign", encoded))
      rc = H2_PAL_ERR_NO_MEMORY;
  }
done:
  if (message != NULL) allocator.free(allocator.ctx, message);
  yyjson_mut_doc_free(business);
  yyjson_doc_free(factory);
  h2_alipay_client_wipe(&private_key, sizeof(private_key));
  h2_alipay_client_wipe(&signature, sizeof(signature));
  h2_alipay_client_wipe(der, sizeof(der));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  if (stored != NULL) {
    h2_alipay_client_wipe(stored, stored_length);
    h2_pal_mem_free(config->mem, stored);
  }
  return rc;
}

int h2_alipay_client_provision_request(
    const h2_alipay_client_config_t* config, const char* appid, bool activate,
    bool* activated, h2_alipay_client_payment_t* diagnostics) {
  if (config == NULL || activated == NULL || diagnostics == NULL) return 99;
  memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->stage =
      activate ? H2_ALIPAY_CLIENT_DEVICE_ACTIVATE : H2_ALIPAY_CLIENT_PROVISION;
  diagnostics->rpc_status = -1;
  if (activate && (*activated || config->activation_asset == NULL ||
                   config->activation_asset_length == 0u ||
                   config->activation_asset_length >= 598u)) {
    diagnostics->result = H2_PAL_ERR_INVALID_STATE;
    return 99;
  }
  client_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  if (work == NULL) {
    diagnostics->result = H2_PAL_ERR_NO_MEMORY;
    return 29;
  }
  memset(work, 0, sizeof(*work));
  yyjson_alc allocator = provision_allocator(config);
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  yyjson_doc* asset = NULL;
  yyjson_doc* response = NULL;
  h2_alipay_client_result_t request = {0};
  request.rpc_status = -1;
  char* body = NULL;
  size_t body_length = 0u, response_length = 0u;
  int rc = h2_alipay_client_load_state(config, &work->state);
  if (rc != H2_PAL_OK) goto done;
  yyjson_mut_val* array = document != NULL ? yyjson_mut_arr(document) : NULL;
  yyjson_mut_val* object = document != NULL ? yyjson_mut_obj(document) : NULL;
  if (array == NULL || object == NULL ||
      !yyjson_mut_arr_add_val(array, object) ||
      !provision_fields(document, object, config, appid, activate)) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  yyjson_mut_doc_set_root(document, array);
  if (activate) {
    yyjson_read_err error;
    asset = yyjson_read_opts((char*)config->activation_asset,
                             config->activation_asset_length, 0u, &allocator,
                             &error);
    if (asset == NULL) {
      rc = error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
      goto done;
    }
    yyjson_mut_val* value =
        yyjson_mut_rawn(document, (const char*)config->activation_asset,
                        config->activation_asset_length);
    if (value == NULL ||
        !yyjson_mut_obj_add_val(document, object, "botInfo", value)) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto done;
    }
  }
  body = yyjson_mut_write_opts(document, 0u, &allocator, &body_length, NULL);
  if (body == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  rc = h2_alipay_client_post(
      config, work, &request, "alipay.mobile.alideviceinfo.device.active",
      false, (const uint8_t*)body, body_length, &response_length);
  if (rc != H2_PAL_OK) goto done;
  yyjson_read_err error;
  response = yyjson_read_opts((char*)work->response, response_length,
                              YYJSON_READ_STOP_WHEN_DONE, &allocator, &error);
  if (response == NULL) {
    rc = error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
             ? H2_PAL_ERR_NO_MEMORY
             : H2_PAL_ERR_FORMAT;
    goto done;
  }
  yyjson_val* root = yyjson_doc_get_root(response);
  if (activate) {
    if (yyjson_is_true(h2_alipay_client_json_member(root, "success")))
      *activated = true;
    else
      rc = H2_PAL_ERR_IO;
  } else {
    const char* certificate =
        yyjson_get_str(h2_alipay_client_json_member(root, "deviceCertificate"));
    if (certificate != NULL)
      (void)h2_alipay_client_provision_receive(config, certificate);
  }
done:
  diagnostics->result = rc;
  diagnostics->requests = request.requests;
  diagnostics->http_status = request.http_status;
  diagnostics->rpc_status = request.rpc_status;
  if (body != NULL) allocator.free(allocator.ctx, body);
  yyjson_doc_free(response);
  yyjson_doc_free(asset);
  yyjson_mut_doc_free(document);
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, work);
  return rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_MEMORY ? 29 : 99;
}
