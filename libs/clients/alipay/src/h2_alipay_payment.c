#include "h2_alipay_client_internal.h"

#include "yyjson.h"
#include "h2_sha256.h"
#include <limits.h>
#include <string.h>

typedef struct payment_workspace {
  client_workspace_t rpc;
  h2_alipay_client_result_t request;
  binding_record_t binding;
} payment_workspace_t;

static int payment_error(h2_alipay_client_payment_t* result, int rc) {
  if (result != NULL) result->result = rc;
  return rc;
}

static payment_workspace_t* create(const h2_alipay_client_config_t* config,
                                   h2_alipay_client_payment_t* result) {
  memset(result, 0, sizeof(*result));
  result->state = H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN;
  result->rpc_status = -1;
  payment_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  if (work != NULL) {
    memset(work, 0, sizeof(*work));
    work->request.rpc_status = -1;
  }
  return work;
}

static void release(const h2_alipay_client_config_t* config,
                    payment_workspace_t* work,
                    h2_alipay_client_payment_t* result) {
  if (work == NULL) return;
  result->requests = work->request.requests;
  result->http_status = work->request.http_status;
  result->rpc_status = work->request.rpc_status;
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, work);
}

static int bound(const h2_alipay_client_config_t* config,
                 payment_workspace_t* work,
                 h2_alipay_client_payment_t* result) {
  result->stage = H2_ALIPAY_CLIENT_STORAGE;
  int rc = h2_alipay_client_load_binding(config, &work->binding);
  if (rc != H2_PAL_OK) return rc;
  return h2_alipay_client_binding_complete(&work->binding)
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_alipay_client_get_paycode(
    const h2_alipay_client_config_t* config, uint8_t* out, size_t* length,
    h2_alipay_client_payment_t* result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->state = H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN;
    result->rpc_status = -1;
  }
  if (out == NULL || length == NULL || result == NULL)
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  size_t capacity = *length;
  *length = 0u;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return payment_error(result, rc);
  payment_workspace_t* work = create(config, result);
  if (work == NULL) return payment_error(result, H2_PAL_ERR_NO_MEMORY);
  rc = bound(config, work, result);
  if (rc == H2_PAL_OK) {
    result->stage = H2_ALIPAY_CLIENT_PAYCODE;
    rc = h2_alipay_paycode_generate(config, &work->binding, out, capacity,
                                    length, &result->stage);
  }
  result->result = rc;
  release(config, work, result);
  return rc;
}

static yyjson_alc allocator(const h2_alipay_client_config_t* config) {
  return (yyjson_alc){h2_alipay_client_json_alloc,
                      h2_alipay_client_json_realloc, h2_alipay_client_json_free,
                      (void*)config->mem};
}

h2_pal_result_t h2_alipay_client_get_collectioncode(
    const h2_alipay_client_config_t* config, uint8_t* out, size_t* length,
    h2_alipay_client_payment_t* result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->state = H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN;
    result->rpc_status = -1;
  }
  if (out == NULL || length == NULL || result == NULL)
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return payment_error(result, rc);
  size_t capacity = *length;
  uint64_t utc_ms = 0u;
  rc = h2_pal_time_get_wall_ms(config->time, &utc_ms);
  if (rc != H2_PAL_OK) return payment_error(result, rc);
  uint64_t seconds = utc_ms / 1000u;
  if (seconds > UINT32_MAX)
    return payment_error(result, H2_PAL_ERR_INVALID_STATE);
  uint8_t payer[20] = {0};
  size_t payer_length = sizeof(payer);
  rc = h2_alipay_client_get_paycode(config, payer, &payer_length, result);
  if (rc == H2_PAL_OK) {
    result->stage = H2_ALIPAY_CLIENT_COLLECTION_CODE;
    char suffix[11];
    bool negative = seconds > INT32_MAX;
    uint32_t magnitude = negative ? (uint32_t)(UINT64_C(4294967296) - seconds)
                                  : (uint32_t)seconds;
    if (negative && magnitude >= 1000000000u) magnitude /= 10u;
    for (size_t index = 10u; index > (negative ? 1u : 0u); --index) {
      suffix[index - 1u] = (char)('0' + magnitude % 10u);
      magnitude /= 10u;
    }
    if (negative) suffix[0] = '-';
    suffix[10] = '\0';
    static const char prefix[] = "CHANGECODE_SQ0";
    size_t size = sizeof(prefix) - 1u + payer_length - 2u + strlen(suffix);
    if (capacity <= size) {
      rc = H2_PAL_ERR_NO_SPACE;
    } else {
      memcpy(out, prefix, sizeof(prefix) - 1u);
      memcpy(out + sizeof(prefix) - 1u, payer + 2u, payer_length - 2u);
      memcpy(out + sizeof(prefix) - 1u + payer_length - 2u, suffix,
             strlen(suffix) + 1u);
    }
    *length = size;
  }
  h2_alipay_client_wipe(payer, sizeof(payer));
  return payment_error(result, rc);
}

static int request(const h2_alipay_client_config_t* config,
                   payment_workspace_t* work, const char* code,
                   const char* token, bool ack, size_t* length) {
  yyjson_alc alc = allocator(config);
  yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
  if (doc == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(doc);
  yyjson_mut_val* object = yyjson_mut_obj(doc);
  yyjson_mut_val* ext = yyjson_mut_obj(doc);
  bool ok =
      array != NULL && object != NULL && ext != NULL &&
      yyjson_mut_obj_add_str(doc, ext, "sdkType", "INSIDE_WATCH") &&
      (!ack || yyjson_mut_obj_add_str(doc, ext, "ackType", "RPC")) &&
      yyjson_mut_obj_add_str(doc, ext, "clientVersion", "4.61") &&
      yyjson_mut_obj_add_str(doc, ext, "sdkHost", config->identity.model) &&
      (ack || token == NULL || token[0] == '\0' ||
       yyjson_mut_obj_add_str(doc, ext, "npayToken", token)) &&
      yyjson_mut_obj_add_str(doc, object, "dynamicId", code) &&
      yyjson_mut_obj_add_str(doc, object, "userId", work->binding.uid) &&
      yyjson_mut_obj_add_str(doc, object, "tid", work->binding.tid) &&
      yyjson_mut_obj_add_val(doc, object, "extInfo", ext) &&
      yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(doc, array);
  size_t size = 0u;
  char* body = ok ? yyjson_mut_write_opts(doc, 0u, &alc, &size, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (body != NULL)
    rc = h2_alipay_client_post(
        config, &work->rpc, &work->request,
        ack ? "alipay.livetradeprod.facepay.sdk.inside.payResultAck"
            : "alipay.livetradeprod.facepay.sdk.inside.queryOrderStatus",
        false, (const uint8_t*)body, size, length);
  h2_alipay_client_json_free((void*)config->mem, body);
  yyjson_mut_doc_free(doc);
  return rc;
}

static int copy_amount(yyjson_val* object, char amount[32]) {
  yyjson_val* value = yyjson_obj_get(object, "realAmount");
  if (value != NULL && !yyjson_is_str(value)) return H2_PAL_ERR_FORMAT;
  if (value == NULL || yyjson_get_len(value) == 0u)
    value = yyjson_obj_get(object, "originAmount");
  if (value == NULL) {
    amount[0] = '\0';
    return H2_PAL_OK;
  }
  if (!yyjson_is_str(value) || yyjson_get_len(value) >= 32u ||
      memchr(yyjson_get_str(value), '\0', yyjson_get_len(value)) != NULL)
    return H2_PAL_ERR_FORMAT;
  memcpy(amount, yyjson_get_str(value), yyjson_get_len(value) + 1u);
  return H2_PAL_OK;
}

static yyjson_doc* parse_string(yyjson_val* value, const yyjson_alc* alc) {
  if (!yyjson_is_str(value)) return NULL;
  return yyjson_read_opts((char*)yyjson_get_str(value), yyjson_get_len(value),
                          0u, alc, NULL);
}

static h2_alipay_client_payment_action_t classify_action(yyjson_val* action) {
  if (action == NULL) return H2_ALIPAY_CLIENT_ACTION_ABSENT;
  if (!yyjson_is_str(action)) return H2_ALIPAY_CLIENT_ACTION_INVALID;
  if (yyjson_equals_str(action, "pay_success"))
    return H2_ALIPAY_CLIENT_ACTION_PAID;
  if (yyjson_equals_str(action, "soundWavePaySuccessPush"))
    return H2_ALIPAY_CLIENT_ACTION_SOUNDWAVE;
  if (yyjson_equals_str(action, "failure_info"))
    return H2_ALIPAY_CLIENT_ACTION_FAILURE;
  if (yyjson_equals_str(action, "CLOSECODE"))
    return H2_ALIPAY_CLIENT_ACTION_DISABLED;
  if (yyjson_equals_str(action, "DELSEED") ||
      yyjson_equals_str(action, "DELSEEDANDINDEX"))
    return H2_ALIPAY_CLIENT_ACTION_UNBOUND;
  return H2_ALIPAY_CLIENT_ACTION_UNKNOWN;
}

static bool diagnostic_code(yyjson_val* value, int* out) {
  if (yyjson_is_sint(value)) {
    int64_t code = yyjson_get_sint(value);
    if (code < 0 || code > INT_MAX) return false;
    *out = (int)code;
    return true;
  }
  if (yyjson_is_uint(value)) {
    uint64_t code = yyjson_get_uint(value);
    if (code > INT_MAX) return false;
    *out = (int)code;
    return true;
  }
  if (!yyjson_is_str(value)) return false;
  size_t size = yyjson_get_len(value);
  if (size == 0u || size > 9u) return false;
  const char* text = yyjson_get_str(value);
  int code = 0;
  for (size_t i = 0u; i < size; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    code = code * 10 + text[i] - '0';
  }
  *out = code;
  return true;
}

static void observe_response(yyjson_val* root, yyjson_val* action,
                             h2_alipay_client_query_diagnostics_t* query) {
  if (yyjson_is_str(action)) {
    const char* text = yyjson_get_str(action);
    size_t size = yyjson_get_len(action);
    h2_sha256_t hash;
    h2_sha256_init(&hash);
    h2_sha256_update(&hash, (const uint8_t*)text, size);
    h2_sha256_finish(&hash, query->action_sha256);
    query->action_hash_valid = true;
    query->action_identifier = size != 0u;
    for (size_t i = 0u; i < size; ++i) {
      bool letter = (text[i] >= 'a' && text[i] <= 'z') ||
                    (text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_';
      bool digit = i != 0u && text[i] >= '0' && text[i] <= '9';
      if (!letter && !digit) query->action_identifier = false;
    }
  }
  yyjson_val* success = yyjson_obj_get(root, "success");
  if (yyjson_is_true(success))
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_SUCCESS_TRUE;
  if (yyjson_is_false(success))
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_SUCCESS_FALSE;
  yyjson_val* error = yyjson_obj_get(root, "errorCode");
  yyjson_val* code = yyjson_obj_get(root, "resultCode");
  if (error != NULL)
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_ERROR_CODE;
  if (yyjson_obj_get(root, "errorMsg") != NULL)
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_ERROR_MESSAGE;
  if (code != NULL)
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_RESULT_CODE;
  if (yyjson_obj_get(root, "resultMsg") != NULL)
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_RESULT_MESSAGE;
  query->response_code_valid =
      diagnostic_code(error != NULL ? error : code, &query->response_code);
  yyjson_val* business = yyjson_obj_get(root, "sdkBizData");
  if (business != NULL) {
    query->response_fields |= H2_ALIPAY_CLIENT_QUERY_BUSINESS;
    query->business_type = yyjson_is_obj(business)   ? 1u
                           : yyjson_is_str(business) ? 2u
                                                     : 3u;
    if (yyjson_is_str(business))
      query->business_length = yyjson_get_len(business);
  }
}

static int parse(const h2_alipay_client_config_t* config,
                 payment_workspace_t* work, size_t response_length,
                 h2_alipay_client_payment_t* result, char* ext_data,
                 size_t capacity, size_t* ext_length, bool details) {
  yyjson_alc alc = allocator(config);
  yyjson_doc* doc = yyjson_read_opts((char*)work->rpc.response, response_length,
                                     0u, &alc, NULL);
  if (doc == NULL) return H2_PAL_ERR_FORMAT;
  yyjson_doc *entire = NULL, *business = NULL;
  yyjson_val* root = yyjson_doc_get_root(doc);
  yyjson_val* envelope = yyjson_obj_get(root, "entireData");
  int rc = H2_PAL_OK;
  if (envelope != NULL) {
    if (yyjson_is_obj(envelope))
      root = envelope;
    else if ((entire = parse_string(envelope, &alc)) != NULL)
      root = yyjson_doc_get_root(entire);
    else {
      rc = H2_PAL_ERR_FORMAT;
      goto done;
    }
  }
  if (!yyjson_is_obj(root)) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  yyjson_val* action = yyjson_obj_get(root, "action");
  observe_response(root, action, &result->query);
  result->query.action = classify_action(action);
  result->query.action_length =
      yyjson_is_str(action) ? yyjson_get_len(action) : 0u;
  if (action == NULL) {
    result->state = H2_ALIPAY_CLIENT_PAYMENT_PENDING;
  } else if (!yyjson_is_str(action)) {
    rc = H2_PAL_ERR_FORMAT;
  } else if (yyjson_equals_str(action, "pay_success") ||
             (details &&
              yyjson_equals_str(action, "soundWavePaySuccessPush"))) {
    yyjson_val* data = root;
    if (yyjson_equals_str(action, "pay_success")) {
      data = yyjson_obj_get(root, "sdkBizData");
      if (yyjson_is_str(data)) {
        business = parse_string(data, &alc);
        data = business != NULL ? yyjson_doc_get_root(business) : NULL;
      }
    }
    if (!yyjson_is_obj(data))
      rc = H2_PAL_ERR_FORMAT;
    else {
      rc = copy_amount(data, result->amount);
      if (rc == H2_PAL_OK) result->state = H2_ALIPAY_CLIENT_PAYMENT_PAID;
    }
  } else if (details && yyjson_equals_str(action, "failure_info")) {
    yyjson_val* data = yyjson_obj_get(root, "sdkBizData");
    if (!yyjson_is_str(data))
      rc = H2_PAL_ERR_FORMAT;
    else {
      size_t size = yyjson_get_len(data);
      *ext_length = size;
      if (ext_data != NULL && size > capacity)
        rc = H2_PAL_ERR_NO_SPACE;
      else {
        if (ext_data != NULL && size != 0u)
          memcpy(ext_data, yyjson_get_str(data), size);
        result->state = H2_ALIPAY_CLIENT_PAYMENT_FAILED;
      }
    }
  } else if (yyjson_equals_str(action, "CLOSECODE")) {
    result->state = H2_ALIPAY_CLIENT_PAYMENT_DISABLED;
  } else if (yyjson_equals_str(action, "DELSEED") ||
             yyjson_equals_str(action, "DELSEEDANDINDEX")) {
    rc = h2_alipay_client_discard_binding(config);
    if (rc == H2_PAL_OK) result->state = H2_ALIPAY_CLIENT_PAYMENT_UNBOUND;
  } else
    result->state = H2_ALIPAY_CLIENT_PAYMENT_PENDING;
done:
  yyjson_doc_free(business);
  yyjson_doc_free(entire);
  yyjson_doc_free(doc);
  return rc;
}

static int query_order(const h2_alipay_client_config_t* config,
                       const char* code, const char* token,
                       h2_alipay_client_payment_t* result, char* ext_data,
                       size_t* ext_length, bool details) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->state = H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN;
    result->rpc_status = -1;
  }
  if (code == NULL || result == NULL || ext_length == NULL ||
      (ext_data == NULL && *ext_length != 0u))
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  size_t capacity = *ext_length;
  *ext_length = 0u;
  size_t size = 0u;
  while (size < 20u && code[size] >= '0' && code[size] <= '9') ++size;
  if ((size != 18u && size != 19u) || code[size] != '\0')
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return payment_error(result, rc);
  payment_workspace_t* work = create(config, result);
  if (work == NULL) return payment_error(result, H2_PAL_ERR_NO_MEMORY);
  rc = bound(config, work, result);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_client_load_state(config, &work->rpc.state);
  if (rc == H2_PAL_OK) {
    size_t response_length = 0u;
    result->stage = H2_ALIPAY_CLIENT_QUERY_PAYMENT;
    result->query.attempted = true;
    result->query.nfc_token_present = token != NULL && token[0] != '\0';
    result->query.code_length = size;
    result->query.tid_length = strlen(work->binding.tid);
    result->query.uid_length = strlen(work->binding.uid);
    rc = request(config, work, code, token, false, &response_length);
    result->query.request_result = rc;
    result->query.http_status = work->request.http_status;
    result->query.rpc_status = work->request.rpc_status;
    result->query.body_length = response_length;
    if (rc == H2_PAL_OK)
      rc = parse(config, work, response_length, result, ext_data, capacity,
                 ext_length, details);
    if (rc == H2_PAL_OK && result->state == H2_ALIPAY_CLIENT_PAYMENT_PAID) {
      /* Match the delivery: an acknowledgement failure does not turn a
       * confirmed business result into a pending or failed transaction. */
      result->ack_result =
          request(config, work, code, NULL, true, &response_length);
    }
  }
  if (rc != H2_PAL_OK) {
    result->state = H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN;
    h2_alipay_client_wipe(result->amount, sizeof(result->amount));
  }
  result->result = rc;
  release(config, work, result);
  return rc;
}

h2_pal_result_t h2_alipay_client_query_order_result(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_payment_t* result, char* ext_data, size_t* ext_length) {
  return query_order(config, code, NULL, result, ext_data, ext_length, true);
}

h2_pal_result_t h2_alipay_client_query_nfc_payment(
    const h2_alipay_client_config_t* config, const char* code,
    const char* token, h2_alipay_client_payment_t* result, char* ext_data,
    size_t* ext_length) {
  if (token == NULL) {
    if (result != NULL) {
      memset(result, 0, sizeof(*result));
      result->rpc_status = -1;
    }
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  }
  return query_order(config, code, token, result, ext_data, ext_length, true);
}

h2_pal_result_t h2_alipay_client_query_order_bitmap(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_payment_t* result, char bitmap[32][14]) {
  if (bitmap == NULL) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  }
  size_t unused_length = 0u;
  int rc = query_order(config, code, NULL, result, NULL, &unused_length, false);
  if (rc == H2_PAL_OK && result->state == H2_ALIPAY_CLIENT_PAYMENT_PAID) {
    memset(bitmap, 0, 4u);
    h2_alipay_amount_bitmap(result->amount, bitmap);
  }
  return rc;
}

static int income_request(const h2_alipay_client_config_t* config,
                          payment_workspace_t* work, const char* code,
                          size_t* length) {
  yyjson_alc alc = allocator(config);
  yyjson_mut_doc* document = yyjson_mut_doc_new(&alc);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(document);
  yyjson_mut_val* object = yyjson_mut_obj(document);
  bool ok =
      array != NULL && object != NULL &&
      yyjson_mut_obj_add_str(document, object, "collectMoneyCode", code) &&
      yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(document, array);
  size_t size = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(document, 0u, &alc, &size, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (body != NULL)
    rc = h2_alipay_client_post(
        config, &work->rpc, &work->request,
        "alipay.mobile.alideviceinfo.changecode.queryIncomeDetailByCode", false,
        (const uint8_t*)body, size, length);
  h2_alipay_client_json_free((void*)config->mem, body);
  yyjson_mut_doc_free(document);
  return rc;
}

static int income_response(const h2_alipay_client_config_t* config,
                           payment_workspace_t* work, size_t length,
                           h2_alipay_client_income_t* out) {
  if (length == 0u) return H2_PAL_ERR_NOT_FOUND;
  yyjson_alc alc = allocator(config);
  yyjson_read_err error;
  yyjson_doc* document =
      yyjson_read_opts((char*)work->rpc.response, length, 0u, &alc, &error);
  if (document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(document);
  int rc = H2_PAL_ERR_FORMAT;
  if (!yyjson_is_obj(root)) goto done;
  if (!yyjson_is_true(yyjson_obj_get(root, "success"))) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  yyjson_val* records = yyjson_obj_get(root, "recordInfos");
  if (!yyjson_is_arr(records) || yyjson_arr_size(records) == 0u) goto done;
  yyjson_val* first = yyjson_arr_get(records, 0u);
  yyjson_val* amount = yyjson_obj_get(first, "amount");
  yyjson_val* remark = yyjson_obj_get(first, "remark");
  if (!yyjson_is_obj(first) || !yyjson_is_str(amount) ||
      strlen(yyjson_get_str(amount)) != yyjson_get_len(amount) ||
      (remark != NULL && !yyjson_is_str(remark)))
    goto done;
  h2_alipay_amount_bitmap(yyjson_get_str(amount), out->bitmap);
  if (remark != NULL) {
    size_t size = yyjson_get_len(remark);
    if (size < sizeof(out->message))
      memset(out->message, 0, sizeof(out->message));
    else
      size = sizeof(out->message);
    memcpy(out->message, yyjson_get_str(remark), size);
  }
  out->success = true;
  rc = H2_PAL_OK;
done:
  yyjson_doc_free(document);
  return rc;
}

h2_pal_result_t h2_alipay_client_query_income(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_income_t* out, h2_alipay_client_payment_t* result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->rpc_status = -1;
  }
  if (out != NULL) out->success = false;
  if (code == NULL || out == NULL || result == NULL)
    return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  size_t size = 0u;
  while (size <= 1024u && code[size] != '\0') ++size;
  if (size > 1024u) return payment_error(result, H2_PAL_ERR_INVALID_ARG);
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return payment_error(result, rc);
  payment_workspace_t* work = create(config, result);
  if (work == NULL) return payment_error(result, H2_PAL_ERR_NO_MEMORY);
  result->stage = H2_ALIPAY_CLIENT_STORAGE;
  rc = h2_alipay_client_load_state(config, &work->rpc.state);
  if (rc == H2_PAL_OK) {
    result->stage = H2_ALIPAY_CLIENT_QUERY_INCOME;
    size_t length = 0u;
    rc = income_request(config, work, code, &length);
    if (rc == H2_PAL_OK) rc = income_response(config, work, length, out);
  }
  result->result = rc;
  release(config, work, result);
  return rc;
}
