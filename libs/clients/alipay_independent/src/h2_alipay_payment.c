#include "h2_alipay_client_internal.h"

#include "yyjson.h"
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
  return work->binding.phase == BINDING_COMPLETE ? H2_PAL_OK
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

static int request(const h2_alipay_client_config_t* config,
                   payment_workspace_t* work, const char* code, bool ack,
                   size_t* length) {
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

static int parse(const h2_alipay_client_config_t* config,
                 payment_workspace_t* work, size_t response_length,
                 h2_alipay_client_payment_t* result, char* ext_data,
                 size_t capacity, size_t* ext_length) {
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
  if (action == NULL) {
    result->state = H2_ALIPAY_CLIENT_PAYMENT_PENDING;
  } else if (!yyjson_is_str(action)) {
    rc = H2_PAL_ERR_FORMAT;
  } else if (yyjson_equals_str(action, "pay_success") ||
             yyjson_equals_str(action, "soundWavePaySuccessPush")) {
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
  } else if (yyjson_equals_str(action, "failure_info")) {
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

h2_pal_result_t h2_alipay_client_query_order_result(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_payment_t* result, char* ext_data, size_t* ext_length) {
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
    rc = request(config, work, code, false, &response_length);
    if (rc == H2_PAL_OK)
      rc = parse(config, work, response_length, result, ext_data, capacity,
                 ext_length);
    if (rc == H2_PAL_OK && result->state == H2_ALIPAY_CLIENT_PAYMENT_PAID) {
      /* Match the delivery: an acknowledgement failure does not turn a
       * confirmed business result into a pending or failed transaction. */
      result->ack_result = request(config, work, code, true, &response_length);
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
