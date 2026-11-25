#include "h2_alipay_scan.h"

#include "h2_alipay_client_internal.h"
#include "yyjson.h"
#include <stdio.h>
#include <string.h>

struct h2_alipay_scan {
  h2_alipay_client_config_t config;
  bool active;
  char uid[65], tid[129], business[128], code[1024];
};

typedef enum scan_request {
  SCAN_PRECHECK,
  SCAN_GENERATE,
  SCAN_QUERY,
  SCAN_CANCEL,
} scan_request_t;

typedef struct scan_workspace {
  client_workspace_t rpc;
  h2_alipay_client_result_t call;
  char uid[65], tid[129];
} scan_workspace_t;

static int error(h2_alipay_client_payment_t* result, int rc) {
  if (result != NULL) result->result = rc;
  return rc;
}

static void initialize_result(h2_alipay_client_payment_t* result) {
  if (result == NULL) return;
  memset(result, 0, sizeof(*result));
  result->rpc_status = -1;
}

static void clear_session(h2_alipay_scan_t* scan) {
  scan->active = false;
  h2_alipay_client_wipe(scan->uid, sizeof(scan->uid));
  h2_alipay_client_wipe(scan->tid, sizeof(scan->tid));
  h2_alipay_client_wipe(scan->business, sizeof(scan->business));
  h2_alipay_client_wipe(scan->code, sizeof(scan->code));
}

h2_pal_result_t h2_alipay_scan_open(const h2_alipay_client_config_t* config,
                                    h2_alipay_scan_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_scan_t* scan = h2_pal_mem_alloc(config->mem, sizeof(*scan));
  if (scan == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(scan, 0, sizeof(*scan));
  scan->config = *config;
  *out = scan;
  return H2_PAL_OK;
}

void h2_alipay_scan_close(h2_alipay_scan_t* scan) {
  if (scan == NULL) return;
  const h2_pal_mem_api_t* memory = scan->config.mem;
  h2_alipay_client_wipe(scan, sizeof(*scan));
  h2_pal_mem_free(memory, scan);
}

static scan_workspace_t* create(h2_alipay_scan_t* scan) {
  scan_workspace_t* work = h2_pal_mem_alloc(scan->config.mem, sizeof(*work));
  if (work != NULL) {
    memset(work, 0, sizeof(*work));
    work->call.rpc_status = -1;
  }
  return work;
}

static int release(h2_alipay_scan_t* scan, scan_workspace_t* work,
                   h2_alipay_client_payment_t* result, int rc) {
  result->requests = work->call.requests;
  result->http_status = work->call.http_status;
  result->rpc_status = work->call.rpc_status;
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(scan->config.mem, work);
  return error(result, rc);
}

static int account(h2_alipay_scan_t* scan, scan_workspace_t* work,
                   h2_alipay_client_payment_t* result) {
  result->stage = H2_ALIPAY_CLIENT_STORAGE;
  binding_record_t record = {0};
  int rc = h2_alipay_client_load_binding(&scan->config, &record);
  if (rc == H2_PAL_OK && !h2_alipay_client_binding_complete(&record))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    memcpy(work->uid, record.uid, strlen(record.uid) + 1u);
    memcpy(work->tid, record.tid, strlen(record.tid) + 1u);
    rc = h2_alipay_client_load_state(&scan->config, &work->rpc.state);
  }
  h2_alipay_client_wipe(&record, sizeof(record));
  return rc;
}

static yyjson_alc allocator(h2_alipay_scan_t* scan) {
  return (yyjson_alc){h2_alipay_client_json_alloc,
                      h2_alipay_client_json_realloc, h2_alipay_client_json_free,
                      (void*)scan->config.mem};
}

static int request(h2_alipay_scan_t* scan, scan_workspace_t* work,
                   scan_request_t kind, const char* amount, size_t* length) {
  static const char* operations[] = {
      "alipay.mobile.alideviceinfo.changecode.device.queryDeviceInfo",
      "alipay.mobile.alideviceinfo.iotpay.scancode.encode",
      "alipay.mobile.alideviceinfo.iotpay.scancode.payResult",
      "alipay.mobile.alideviceinfo.iotpay.scancode.invalidOutBizNo"};
  yyjson_alc alc = allocator(scan);
  yyjson_mut_doc* document = yyjson_mut_doc_new(&alc);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(document);
  yyjson_mut_val* object = yyjson_mut_obj(document);
  bool ok = array != NULL && object != NULL;
  if (kind == SCAN_PRECHECK) {
    yyjson_mut_val* businesses = yyjson_mut_arr(document);
    ok = ok && businesses != NULL &&
         yyjson_mut_obj_add_str(document, object, "tid", work->tid) &&
         yyjson_mut_obj_add_str(document, object, "userid", work->uid) &&
         yyjson_mut_obj_add_str(document, object, "version", "4.61") &&
         yyjson_mut_arr_add_str(document, businesses, "CODE_TYPE") &&
         yyjson_mut_obj_add_val(document, object, "queryBizTypeList",
                                businesses);
  } else if (kind != SCAN_CANCEL) {
    ok = ok && yyjson_mut_obj_add_str(document, object, "userId", scan->uid) &&
         yyjson_mut_obj_add_str(document, object, "tid", scan->tid);
    if (kind == SCAN_GENERATE)
      ok = ok && yyjson_mut_obj_add_str(document, object, "amount", amount);
    else
      ok = ok &&
           yyjson_mut_obj_add_str(document, object, "qrCode", scan->code) &&
           yyjson_mut_obj_add_str(document, object, "bizType", "watchPayCode");
  }
  if (kind != SCAN_PRECHECK)
    ok = ok &&
         yyjson_mut_obj_add_str(document, object, "outBizNo", scan->business);
  ok = ok && yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(document, array);
  size_t size = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(document, 0u, &alc, &size, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (body != NULL)
    rc = h2_alipay_client_post(&scan->config, &work->rpc, &work->call,
                               operations[kind], false, (const uint8_t*)body,
                               size, length);
  h2_alipay_client_json_free((void*)scan->config.mem, body);
  yyjson_mut_doc_free(document);
  return rc;
}

static int response(h2_alipay_scan_t* scan, scan_workspace_t* work,
                    size_t length, yyjson_doc** out) {
  *out = NULL;
  if (length == 0u) return H2_PAL_ERR_NOT_FOUND;
  yyjson_alc alc = allocator(scan);
  yyjson_read_err issue;
  *out = yyjson_read_opts((char*)work->rpc.response, length, 0u, &alc, &issue);
  if (*out == NULL)
    return issue.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(*out);
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) > 64u)
    return H2_PAL_ERR_FORMAT;
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

h2_pal_result_t h2_alipay_scan_dzs_precheck(
    h2_alipay_scan_t* scan, uint8_t* enabled,
    h2_alipay_client_payment_t* result) {
  initialize_result(result);
  if (enabled != NULL) *enabled = 0u;
  if (scan == NULL || enabled == NULL || result == NULL)
    return error(result, H2_PAL_ERR_INVALID_ARG);
  scan_workspace_t* work = create(scan);
  if (work == NULL) return error(result, H2_PAL_ERR_NO_MEMORY);
  int rc = account(scan, work, result);
  yyjson_doc* document = NULL;
  if (rc == H2_PAL_OK) {
    result->stage = H2_ALIPAY_CLIENT_DZS_PRECHECK;
    size_t length = 0u;
    rc = request(scan, work, SCAN_PRECHECK, NULL, &length);
    if (rc == H2_PAL_OK) rc = response(scan, work, length, &document);
    if (rc == H2_PAL_OK) {
      yyjson_val* root = yyjson_doc_get_root(document);
      yyjson_val* display = yyjson_obj_get(root, "shopkeeperSweepDisplay");
      if (!yyjson_is_true(yyjson_obj_get(root, "success")) || display == NULL)
        rc = H2_PAL_ERR_INVALID_STATE;
      else
        *enabled = yyjson_is_true(display) ? 1u : 0u;
    }
  }
  yyjson_doc_free(document);
  return release(scan, work, result, rc);
}

h2_pal_result_t h2_alipay_scan_dzs_generate(
    h2_alipay_scan_t* scan, const char* amount, h2_alipay_scan_code_t* out,
    h2_alipay_client_payment_t* result) {
  initialize_result(result);
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
    out->failure = H2_ALIPAY_SCAN_COMMON_FAILURE;
  }
  if (scan == NULL || amount == NULL || out == NULL || result == NULL)
    return error(result, H2_PAL_ERR_INVALID_ARG);
  size_t amount_length = 0u;
  while (amount_length < 32u && amount[amount_length] != '\0') ++amount_length;
  if (amount_length == 32u) return error(result, H2_PAL_ERR_INVALID_ARG);
  clear_session(scan);
  scan_workspace_t* work = create(scan);
  if (work == NULL) return error(result, H2_PAL_ERR_NO_MEMORY);
  int rc = account(scan, work, result);
  yyjson_doc* document = NULL;
  if (rc == H2_PAL_OK && strlen(work->uid) > 32u) rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK) {
    memcpy(scan->uid, work->uid, strlen(work->uid) + 1u);
    memcpy(scan->tid, work->tid, strlen(work->tid) + 1u);
    result->stage = H2_ALIPAY_CLIENT_DZS_GENERATE;
    uint64_t milliseconds = 0u;
    rc = h2_pal_time_get_wall_ms(scan->config.time, &milliseconds);
    uint64_t seconds = milliseconds / 1000u;
    if (rc == H2_PAL_OK && seconds > UINT32_MAX) rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) {
      int64_t timestamp = seconds <= INT32_MAX
                              ? (int64_t)seconds
                              : (int64_t)seconds - INT64_C(4294967296);
      int size = snprintf(scan->business, sizeof(scan->business),
                          "%s%lld0123456789", scan->uid, (long long)timestamp);
      if (size < 0 || (size_t)size >= sizeof(scan->business))
        rc = H2_PAL_ERR_FORMAT;
    }
    size_t length = 0u;
    if (rc == H2_PAL_OK)
      rc = request(scan, work, SCAN_GENERATE, amount, &length);
    if (rc == H2_PAL_OK) rc = response(scan, work, length, &document);
    if (rc == H2_PAL_OK) {
      yyjson_val* root = yyjson_doc_get_root(document);
      yyjson_val* success = yyjson_obj_get(root, "success");
      if (!yyjson_is_bool(success)) {
        rc = H2_PAL_ERR_FORMAT;
      } else if (yyjson_is_true(success)) {
        yyjson_val* code = yyjson_obj_get(root, "qrcode");
        if (!yyjson_is_str(code) ||
            yyjson_get_len(code) >= sizeof(scan->code) ||
            strlen(yyjson_get_str(code)) != yyjson_get_len(code))
          rc = H2_PAL_ERR_FORMAT;
        else {
          out->success = true;
          out->length = yyjson_get_len(code);
          memcpy(out->code, yyjson_get_str(code), out->length + 1u);
          memcpy(scan->code, out->code, out->length + 1u);
          scan->active = out->length != 0u;
        }
      } else {
        yyjson_val* code = yyjson_obj_get(root, "errorCode");
        if (!yyjson_is_str(code))
          rc = H2_PAL_ERR_FORMAT;
        else if (yyjson_equals_str(code, "2084"))
          out->failure = H2_ALIPAY_SCAN_BALANCE_INSUFFICIENT;
        else if (yyjson_equals_str(code, "1007"))
          out->failure = H2_ALIPAY_SCAN_ACCOUNT_MISSING;
        else if (yyjson_equals_str(code, "2086"))
          out->failure = H2_ALIPAY_SCAN_AMOUNT_LIMIT;
      }
    }
  }
  yyjson_doc_free(document);
  return release(scan, work, result, rc);
}

h2_pal_result_t h2_alipay_scan_dzs_query(h2_alipay_scan_t* scan,
                                         h2_alipay_client_payment_t* result) {
  initialize_result(result);
  if (scan == NULL || result == NULL)
    return error(result, H2_PAL_ERR_INVALID_ARG);
  result->stage = H2_ALIPAY_CLIENT_DZS_QUERY;
  if (!scan->active) return error(result, H2_PAL_ERR_NOT_FOUND);
  scan_workspace_t* work = create(scan);
  if (work == NULL) return error(result, H2_PAL_ERR_NO_MEMORY);
  int rc = h2_alipay_client_load_state(&scan->config, &work->rpc.state);
  yyjson_doc* document = NULL;
  size_t length = 0u;
  if (rc == H2_PAL_OK) rc = request(scan, work, SCAN_QUERY, NULL, &length);
  if (rc == H2_PAL_OK) rc = response(scan, work, length, &document);
  if (rc == H2_PAL_OK) {
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* success = yyjson_obj_get(root, "success");
    yyjson_val* code = yyjson_obj_get(root, "resultCode");
    if (!yyjson_is_bool(success) ||
        (yyjson_is_true(success) && !yyjson_is_str(code)))
      rc = H2_PAL_ERR_INVALID_STATE;
    else if (yyjson_is_true(success)) {
      if (yyjson_equals_str(code, "SUCCESS"))
        result->state = H2_ALIPAY_CLIENT_PAYMENT_PAID;
      else if (yyjson_equals_str(code, "INPROCESS"))
        result->state = H2_ALIPAY_CLIENT_PAYMENT_PROCESSING;
    }
  }
  yyjson_doc_free(document);
  return release(scan, work, result, rc);
}

h2_pal_result_t h2_alipay_scan_dzs_cancel(h2_alipay_scan_t* scan,
                                          h2_alipay_client_payment_t* result) {
  initialize_result(result);
  if (scan == NULL || result == NULL)
    return error(result, H2_PAL_ERR_INVALID_ARG);
  result->stage = H2_ALIPAY_CLIENT_DZS_CANCEL;
  if (!scan->active) return H2_PAL_OK;
  scan_workspace_t* work = create(scan);
  if (work == NULL) return error(result, H2_PAL_ERR_NO_MEMORY);
  int rc = h2_alipay_client_load_state(&scan->config, &work->rpc.state);
  size_t length = 0u;
  if (rc == H2_PAL_OK) rc = request(scan, work, SCAN_CANCEL, NULL, &length);
  if (work->call.requests != 0u) clear_session(scan);
  return release(scan, work, result, rc);
}
