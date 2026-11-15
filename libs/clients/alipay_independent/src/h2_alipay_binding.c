#include "h2_alipay_client_internal.h"

#include "h2_sha256.h"
#include "yyjson.h"
#include <string.h>

/* One Pref value is the transaction boundary. The digest detects corruption;
 * confidentiality/authenticity are the injected store's responsibility. */
static int pref_read(const h2_alipay_client_config_t* config, void** raw,
                     size_t* length) {
  *raw = NULL;
  *length = 0u;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc != H2_PAL_OK) return rc < 0 ? rc : H2_PAL_ERR_IO;
  if (ns == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = ns->get_blob != NULL
           ? ns->get_blob(ns, config->mem, "binding", raw, length)
           : H2_PAL_ERR_UNSUPPORTED;
  return h2_alipay_client_finish_namespace(ns, rc);
}

static void release_blob(const h2_alipay_client_config_t* config, void* raw,
                         size_t length) {
  if (raw != NULL) h2_alipay_client_wipe(raw, length);
  h2_pal_mem_free(config->mem, raw);
}

static void digest(const uint8_t* data, size_t length, uint8_t out[32]) {
  h2_sha256_t hash;
  h2_sha256_init(&hash);
  h2_sha256_update(&hash, data, length);
  h2_sha256_finish(&hash, out);
}

int h2_alipay_client_load_binding(const h2_alipay_client_config_t* config,
                                  binding_record_t* record) {
  void* raw = NULL;
  size_t length = 0u;
  int rc = pref_read(config, &raw, &length);
  if (rc != H2_PAL_OK) goto done;
  const uint8_t* data = raw;
  uint8_t expected[32];
  if (data == NULL || length < 113u || length > BINDING_BYTES_MAX ||
      memcmp(data, "APB1", 4u) != 0 ||
      (data[36] != BINDING_STAGED && data[36] != BINDING_COMPLETE)) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  digest(data, length - 32u, expected);
  if (memcmp(expected, data + length - 32u, 32u) != 0) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  h2_alipay_client_fingerprint(config, expected);
  if (memcmp(expected, data + 4u, 32u) != 0) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  void* fields[] = {record->token,    record->tid,   record->uid,
                    record->nickname, record->login, record->seed};
  const size_t limits[] = {BINDING_TOKEN_MAX, BINDING_ID_MAX,
                           BINDING_ID_MAX,    BINDING_TEXT_MAX,
                           BINDING_TEXT_MAX,  BINDING_SEED_MAX};
  size_t offset = 49u;
  for (size_t i = 0u; i < 6u; ++i) {
    size_t size =
        (size_t)data[37u + i * 2u] | ((size_t)data[38u + i * 2u] << 8u);
    if (size > limits[i] || offset > length - 64u ||
        size > length - 64u - offset ||
        (i < 5u && memchr(data + offset, '\0', size) != NULL) ||
        ((i == 1u || i == 2u) && size == 0u) || (i == 5u && size < 16u) ||
        (i == 0u && ((data[36] == BINDING_STAGED && size == 0u) ||
                     (data[36] == BINDING_COMPLETE && size != 0u)))) {
      rc = H2_PAL_ERR_FORMAT;
      goto done;
    }
    memcpy(fields[i], data + offset, size);
    if (i == 5u) record->seed_length = size;
    offset += size;
  }
  if (offset != length - 64u) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  memcpy(record->secret, data + offset, 32u);
  record->phase = data[36];
done:
  release_blob(config, raw, length);
  if (rc != H2_PAL_OK) h2_alipay_client_wipe(record, sizeof(*record));
  return rc;
}

static int save_binding(const h2_alipay_client_config_t* config,
                        const binding_record_t* record) {
  uint8_t* data = h2_pal_mem_alloc(config->mem, BINDING_BYTES_MAX);
  if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(data, 0, BINDING_BYTES_MAX);
  memcpy(data, "APB1", 4u);
  h2_alipay_client_fingerprint(config, data + 4u);
  data[36] = record->phase;
  const void* fields[] = {record->token,    record->tid,   record->uid,
                          record->nickname, record->login, record->seed};
  const size_t sizes[] = {
      record->phase == BINDING_STAGED ? strlen(record->token) : 0u,
      strlen(record->tid),
      strlen(record->uid),
      strlen(record->nickname),
      strlen(record->login),
      record->seed_length};
  size_t offset = 49u;
  for (size_t i = 0u; i < 6u; ++i) {
    data[37u + i * 2u] = (uint8_t)sizes[i];
    data[38u + i * 2u] = (uint8_t)(sizes[i] >> 8u);
    memcpy(data + offset, fields[i], sizes[i]);
    offset += sizes[i];
  }
  memcpy(data + offset, record->secret, 32u);
  offset += 32u;
  digest(data, offset, data + offset);
  offset += 32u;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    rc = ns->set_blob != NULL && ns->commit != NULL
             ? ns->set_blob(ns, "binding", data, offset)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK) rc = ns->commit(ns);
    rc = h2_alipay_client_finish_namespace(ns, rc);
  }
  if (rc == H2_PAL_OK) {
    void* readback = NULL;
    size_t size = 0u;
    rc = pref_read(config, &readback, &size);
    if (rc == H2_PAL_OK && (readback == NULL || size != offset ||
                            memcmp(data, readback, offset) != 0))
      rc = H2_PAL_ERR_IO;
    release_blob(config, readback, size);
  }
  h2_alipay_client_wipe(data, BINDING_BYTES_MAX);
  h2_pal_mem_free(config->mem, data);
  return rc <= 0 ? rc : H2_PAL_ERR_IO;
}

static void describe(const binding_record_t* record,
                     h2_alipay_client_binding_t* out) {
  out->tid_length = strlen(record->tid);
  out->uid_length = strlen(record->uid);
  out->shared_key_length = 32u;
  out->seed_length = record->seed_length;
  out->state = record->phase == BINDING_COMPLETE
                   ? H2_ALIPAY_CLIENT_COMPLETE
                   : H2_ALIPAY_CLIENT_NEEDS_VERIFICATION;
}

h2_pal_result_t h2_alipay_client_binding_snapshot(
    const h2_alipay_client_config_t* config, h2_alipay_client_binding_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_client_binding_t){.state =
                                          H2_ALIPAY_CLIENT_NEEDS_VERIFICATION};
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  binding_record_t* record = h2_pal_mem_alloc(config->mem, sizeof(*record));
  if (record == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(record, 0, sizeof(*record));
  rc = h2_alipay_client_load_binding(config, record);
  if (rc == H2_PAL_ERR_NOT_FOUND) {
    out->state = H2_ALIPAY_CLIENT_UNBOUND;
    rc = H2_PAL_OK;
  } else if (rc == H2_PAL_OK) {
    describe(record, out);
  }
  out->storage_rc = rc;
  h2_alipay_client_wipe(record, sizeof(*record));
  h2_pal_mem_free(config->mem, record);
  return rc;
}

static yyjson_alc json_allocator(const h2_alipay_client_config_t* config) {
  return (yyjson_alc){h2_alipay_client_json_alloc,
                      h2_alipay_client_json_realloc, h2_alipay_client_json_free,
                      (void*)config->mem};
}

static int read_json(const h2_alipay_client_config_t* config,
                     client_workspace_t* workspace, size_t length,
                     yyjson_doc** out) {
  yyjson_alc allocator = json_allocator(config);
  yyjson_read_err error;
  *out = yyjson_read_opts((char*)workspace->response, length, 0u, &allocator,
                          &error);
  if (*out == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(*out);
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) > 64u)
    return H2_PAL_ERR_FORMAT;
  /* Reject ambiguous duplicate names, including success/needQuery. */
  yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
  yyjson_val* key;
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char* name = yyjson_get_str(key);
    if (strlen(name) != yyjson_get_len(key) ||
        yyjson_obj_get(root, name) != yyjson_obj_iter_get_val(key))
      return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int token_request(const h2_alipay_client_config_t* config,
                         client_workspace_t* workspace,
                         h2_alipay_client_result_t* session, const char* token,
                         bool finish, size_t* length) {
  yyjson_alc allocator = json_allocator(config);
  yyjson_mut_doc* doc = yyjson_mut_doc_new(&allocator);
  if (doc == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(doc);
  yyjson_mut_val* object = yyjson_mut_obj(doc);
  bool ok = array != NULL && object != NULL &&
            yyjson_mut_arr_append(array, object) &&
            yyjson_mut_obj_add_str(doc, object, "token", token) &&
            yyjson_mut_obj_add_str(doc, object, "operationMode", "NETWORK");
  if (finish)
    ok = ok && yyjson_mut_obj_add_str(doc, object, "seRes", "") &&
         yyjson_mut_obj_add_str(doc, object, "operateRes", "success");
  yyjson_mut_doc_set_root(doc, array);
  size_t size = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(doc, 0u, &allocator, &size, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (body != NULL)
    rc = h2_alipay_client_post(
        config, workspace, session,
        finish ? "alipay.mobile.alideviceinfo.changecode.finishBind"
               : "alipay.mobile.alideviceinfo.changecode.queryBindInfo",
        false, (const uint8_t*)body, size, length);
  h2_alipay_client_json_free((void*)config->mem, body);
  yyjson_mut_doc_free(doc);
  return rc;
}

static int text(yyjson_val* root, const char* name, char* out, size_t capacity,
                bool empty) {
  yyjson_val* value = yyjson_obj_get(root, name);
  size_t length = yyjson_get_len(value);
  if (!yyjson_is_str(value) || length >= capacity || (!empty && length == 0u) ||
      memchr(yyjson_get_str(value), '\0', length) != NULL)
    return H2_PAL_ERR_FORMAT;
  memcpy(out, yyjson_get_str(value), length + 1u);
  return H2_PAL_OK;
}

static int hex_digit(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  return -1;
}

static int decode_hex(const char* source, size_t length, uint8_t* out) {
  if ((length & 1u) != 0u) return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < length; i += 2u) {
    int high = hex_digit((unsigned char)source[i]);
    int low = hex_digit((unsigned char)source[i + 1u]);
    if (high < 0 || low < 0) return H2_PAL_ERR_FORMAT;
    out[i / 2u] = (uint8_t)((high << 4) | low);
  }
  return H2_PAL_OK;
}

static int factors(const h2_alipay_client_config_t* config, yyjson_val* root,
                   const h2_pal_p256_private_key_t* private_key,
                   binding_record_t* record) {
  yyjson_val* value = yyjson_obj_get(root, "encryptedFactors");
  size_t size = yyjson_get_len(value);
  if (!yyjson_is_str(value) || size < 161u ||
      size > 129u + 2u * BINDING_SEED_MAX)
    return H2_PAL_ERR_FORMAT;
  const char* encoded = yyjson_get_str(value);
  if (encoded[128] != '&' || memchr(encoded, '\0', size) != NULL)
    return H2_PAL_ERR_FORMAT;
  uint8_t point[64], uncompressed[65];
  int rc = decode_hex(encoded, 128u, point);
  if (rc == H2_PAL_OK)
    rc = decode_hex(encoded + 129u, size - 129u, record->seed);
  if (rc != H2_PAL_OK) return rc;
  record->seed_length = (size - 129u) / 2u;
  h2_pal_p256_public_key_t public_key;
  public_key.bytes[0] = (uint8_t)(2u | (point[63] & 1u));
  memcpy(public_key.bytes + 1u, point, 32u);
  rc = h2_pal_crypto_p256_public_key_uncompress(config->crypto, &public_key,
                                                uncompressed);
  if (rc != H2_PAL_OK) return rc;
  /* Validate both supplied coordinates, not just X and the Y parity bit. */
  if (memcmp(uncompressed + 1u, point, 64u) != 0) return H2_PAL_ERR_FORMAT;
  return h2_pal_crypto_p256_shared_secret(config->crypto, private_key,
                                          &public_key, record->secret);
}

h2_pal_result_t h2_alipay_client_query_binding_result(
    const h2_alipay_client_config_t* config, h2_alipay_client_result_t* session,
    h2_alipay_client_binding_t* out) {
  if (out == NULL || session == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_client_binding_t){.state = H2_ALIPAY_CLIENT_PENDING};
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  session->requests = 0u;
  session->http_status = 0;
  session->rpc_status = -1;
  session->stage = H2_ALIPAY_CLIENT_STORAGE;
  client_workspace_t* workspace =
      h2_pal_mem_alloc(config->mem, sizeof(*workspace));
  binding_record_t* record = h2_pal_mem_alloc(config->mem, sizeof(*record));
  yyjson_doc* doc = NULL;
  size_t length = 0u;
  if (workspace == NULL || record == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  memset(workspace, 0, sizeof(*workspace));
  memset(record, 0, sizeof(*record));
  rc = h2_alipay_client_load_binding(config, record);
  if (rc == H2_PAL_OK && record->phase == BINDING_COMPLETE) {
    describe(record, out);
    goto done;
  }
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
    out->storage_rc = rc;
    out->state = H2_ALIPAY_CLIENT_NEEDS_VERIFICATION;
    goto done;
  }
  bool staged = rc == H2_PAL_OK;
  if (staged)
    h2_alipay_client_wipe(&session->private_key, sizeof(session->private_key));
  if (!staged && session->token[0] == '\0') {
    rc = H2_PAL_ERR_INVALID_STATE;
    out->state = H2_ALIPAY_CLIENT_FAILED;
    goto done;
  }
  rc = h2_alipay_client_load_state(config, &workspace->state);
  if (rc != H2_PAL_OK) {
    out->storage_rc = rc;
    out->state = H2_ALIPAY_CLIENT_NEEDS_VERIFICATION;
    goto done;
  }
  if (!staged) {
    session->stage = H2_ALIPAY_CLIENT_QUERY_BIND;
    rc = token_request(config, workspace, session, session->token, false,
                       &length);
    if (rc != H2_PAL_OK) goto done;
    rc = read_json(config, workspace, length, &doc);
    if (rc != H2_PAL_OK) goto malformed;
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* need = yyjson_obj_get(root, "needQuery");
    if (!yyjson_is_bool(need)) {
      rc = H2_PAL_ERR_FORMAT;
      goto malformed;
    }
    if (yyjson_is_true(need)) goto done;
    yyjson_val* success = yyjson_obj_get(root, "success");
    if (!yyjson_is_true(success)) {
      rc = H2_PAL_ERR_FORMAT;
      goto malformed;
    }
    char server_time[33];
    rc = text(root, "serverTime", server_time, sizeof(server_time), false);
    if (rc == H2_PAL_OK)
      rc = text(root, "tid", record->tid, sizeof(record->tid), false);
    if (rc == H2_PAL_OK)
      rc = text(root, "userId", record->uid, sizeof(record->uid), false);
    if (rc == H2_PAL_OK)
      rc = text(root, "nickName", record->nickname, sizeof(record->nickname),
                true);
    if (rc == H2_PAL_OK)
      rc = text(root, "loginId", record->login, sizeof(record->login), true);
    if (rc != H2_PAL_OK) goto malformed;
    session->stage = H2_ALIPAY_CLIENT_FACTORS;
    rc = factors(config, root, &session->private_key, record);
    if (rc != H2_PAL_OK) goto malformed;
    memcpy(record->token, session->token, sizeof(record->token));
    record->phase = BINDING_STAGED;
    session->stage = H2_ALIPAY_CLIENT_BINDING_COMMIT;
    rc = save_binding(config, record);
    if (rc != H2_PAL_OK) goto storage_failure;
    h2_alipay_client_wipe(&session->private_key, sizeof(session->private_key));
    yyjson_doc_free(doc);
    doc = NULL;
  }
  /* Acknowledged finish is only reused for the same staged token. */
  if (!session->finish_acknowledged ||
      strcmp(session->token, record->token) != 0) {
    session->stage = H2_ALIPAY_CLIENT_FINISH_BIND;
    rc =
        token_request(config, workspace, session, record->token, true, &length);
    if (rc != H2_PAL_OK) goto done;
    rc = read_json(config, workspace, length, &doc);
    if (rc != H2_PAL_OK) goto malformed;
    if (!yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "success"))) {
      rc = H2_PAL_ERR_FORMAT;
      goto malformed;
    }
    memcpy(session->token, record->token, sizeof(session->token));
    session->finish_acknowledged = true;
  }
  session->stage = H2_ALIPAY_CLIENT_BINDING_COMMIT;
  record->phase = BINDING_COMPLETE;
  rc = save_binding(config, record);
  if (rc != H2_PAL_OK) goto storage_failure;
  describe(record, out);
  goto done;
malformed:
  out->state = H2_ALIPAY_CLIENT_FAILED;
  goto done;
storage_failure:
  out->state = H2_ALIPAY_CLIENT_NEEDS_VERIFICATION;
  out->storage_rc = rc;
done:
  if (rc != H2_PAL_OK && out->state == H2_ALIPAY_CLIENT_PENDING &&
      session->stage != H2_ALIPAY_CLIENT_QUERY_BIND &&
      session->stage != H2_ALIPAY_CLIENT_FINISH_BIND)
    out->state = H2_ALIPAY_CLIENT_FAILED;
  yyjson_doc_free(doc);
  if (out->state == H2_ALIPAY_CLIENT_COMPLETE) {
    h2_alipay_client_wipe(&session->private_key, sizeof(session->private_key));
    h2_alipay_client_wipe(session->token, sizeof(session->token));
    h2_alipay_client_wipe(session->code, sizeof(session->code));
    session->length = 0u;
    session->finish_acknowledged = false;
  }
  if (record != NULL) h2_alipay_client_wipe(record, sizeof(*record));
  if (workspace != NULL) h2_alipay_client_wipe(workspace, sizeof(*workspace));
  h2_pal_mem_free(config->mem, record);
  h2_pal_mem_free(config->mem, workspace);
  return rc;
}

int h2_alipay_client_discard_binding(const h2_alipay_client_config_t* config) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK) return rc < 0 ? rc : H2_PAL_ERR_IO;
  if (ns == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = ns->remove != NULL && ns->commit != NULL ? ns->remove(ns, "binding")
                                                : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  if (rc == H2_PAL_OK) rc = ns->commit(ns);
  return h2_alipay_client_finish_namespace(ns, rc);
}
