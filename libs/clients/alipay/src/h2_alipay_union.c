#include "h2_alipay_scan.h"

#include "h2_alipay_client_internal.h"
#include "h2_sha256.h"
#include "yyjson.h"
#include <stdio.h>
#include <string.h>

typedef enum union_kind {
  UNION_NONE,
  UNION_TRANSFER,
  UNION_PROXY,
  UNION_AGGREGATE,
  UNION_PARENT,
} union_kind_t;

struct h2_alipay_union {
  h2_alipay_client_config_t config;
  union_kind_t kind;
  bool submitted;
  char uid[65], tid[65], code[1024], receiver[65], app[129], trade[129];
  char business[128];
  uint8_t account_fingerprint[32];
  h2_alipay_scan_order_t order;
};

typedef struct union_work {
  client_workspace_t rpc;
  h2_alipay_client_result_t call;
  yyjson_doc* response;
  char uri[4096], temporary[2048], decoded[1024];
} union_work_t;

typedef struct union_field {
  const char* key;
  const char* value;
} union_field_t;

static yyjson_alc allocator(const h2_alipay_union_t* session) {
  return (yyjson_alc){h2_alipay_client_json_alloc,
                      h2_alipay_client_json_realloc, h2_alipay_client_json_free,
                      (void*)session->config.mem};
}

static bool text(const char* value, size_t capacity, size_t* length) {
  if (value == NULL) return false;
  for (*length = 0u; *length < capacity; ++*length)
    if (value[*length] == '\0') return true;
  return false;
}

static int copy_text(char* out, size_t capacity, const char* input) {
  size_t length = 0u;
  if (!text(input, capacity, &length)) return H2_PAL_ERR_NO_SPACE;
  memcpy(out, input, length + 1u);
  return H2_PAL_OK;
}

static int field(yyjson_val* object, const char* key, char* out,
                 size_t capacity, bool required) {
  yyjson_val* value = yyjson_obj_get(object, key);
  if (value == NULL && !required) return H2_PAL_OK;
  if (!yyjson_is_str(value) ||
      strlen(yyjson_get_str(value)) != yyjson_get_len(value))
    return H2_PAL_ERR_FORMAT;
  return copy_text(out, capacity, yyjson_get_str(value));
}

static unsigned hex_digit(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10u;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10u;
  return 16u;
}

int h2_alipay_client_url_parameter(const char* url, const char* key, char* out,
                                   size_t capacity) {
  out[0] = '\0';
  const char* cursor = strchr(url, '?');
  if (cursor == NULL) return H2_PAL_ERR_NOT_FOUND;
  ++cursor;
  size_t key_length = strlen(key);
  while (*cursor != '\0' && *cursor != '#') {
    const char* end = strchr(cursor, '&');
    if (end == NULL) end = cursor + strlen(cursor);
    if ((size_t)(end - cursor) > key_length &&
        memcmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
      cursor += key_length + 1u;
      size_t length = 0u;
      while (cursor < end && *cursor != '#') {
        unsigned char byte = (unsigned char)*cursor++;
        if (byte == '%') {
          if (end - cursor < 2) return H2_PAL_ERR_FORMAT;
          unsigned high = hex_digit((unsigned char)cursor[0]);
          unsigned low = hex_digit((unsigned char)cursor[1]);
          if (high > 15u || low > 15u) return H2_PAL_ERR_FORMAT;
          byte = (unsigned char)((high << 4u) | low);
          cursor += 2;
        } else if (byte == '+') {
          byte = ' ';
        }
        if (byte == 0u || length + 1u >= capacity) return H2_PAL_ERR_NO_SPACE;
        out[length++] = (char)byte;
      }
      out[length] = '\0';
      return H2_PAL_OK;
    }
    cursor = *end == '&' ? end + 1 : end;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static bool add_fields(yyjson_mut_doc* document, yyjson_mut_val* object,
                       const union_field_t* fields, size_t count) {
  if (object == NULL) return false;
  for (size_t index = 0u; index < count; ++index)
    if (!yyjson_mut_obj_add_str(document, object, fields[index].key,
                                fields[index].value))
      return false;
  return true;
}

static int post(h2_alipay_union_t* session, union_work_t* work,
                const char* operation, const union_field_t* fields,
                size_t count, bool route, bool extensions) {
  yyjson_doc_free(work->response);
  work->response = NULL;
  yyjson_alc alc = allocator(session);
  yyjson_mut_doc* document = yyjson_mut_doc_new(&alc);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(document);
  yyjson_mut_val* object = yyjson_mut_obj(document);
  bool ok = array != NULL && add_fields(document, object, fields, count);
  if (route) {
    yyjson_mut_val* decode = yyjson_mut_obj(document);
    yyjson_mut_val* extra = yyjson_mut_obj(document);
    yyjson_mut_val* product = yyjson_mut_obj(document);
    ok = ok && decode != NULL && extra != NULL && product != NULL &&
         yyjson_mut_obj_add_str(document, decode, "code", session->code) &&
         yyjson_mut_obj_add_str(document, extra, "userId", session->uid) &&
         yyjson_mut_obj_add_val(document, object, "decodeData", decode) &&
         yyjson_mut_obj_add_val(document, object, "extData", extra) &&
         yyjson_mut_obj_add_str(document, object, "paiType", "qrCode") &&
         yyjson_mut_obj_add_str(document, product, "product",
                                "product_wearable") &&
         yyjson_mut_obj_add_str(document, product, "productVersion",
                                "10.3.9.240418") &&
         yyjson_mut_obj_add_val(document, object, "productContext", product);
  }
  if (extensions) {
    yyjson_mut_val* extra = yyjson_mut_obj(document);
    ok = ok && extra != NULL &&
         yyjson_mut_obj_add_str(document, extra, "ua", "QZ_watch") &&
         yyjson_mut_obj_add_str(document, extra, "netType", "4G") &&
         yyjson_mut_obj_add_val(document, object, "extInfo", extra);
    if (session->kind == UNION_TRANSFER)
      ok =
          ok &&
          yyjson_mut_obj_add_str(document, object, "bizType", "TransferCode") &&
          yyjson_mut_obj_add_str(document, object, "receiverUid",
                                 session->receiver);
    else
      ok = ok &&
           yyjson_mut_obj_add_str(document, object, "bizType", "UnionCode");
  }
  ok = ok && yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(document, array);
  size_t length = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(document, 0u, &alc, &length, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  size_t response_length = 0u;
  if (body != NULL)
    rc = h2_alipay_client_post(&session->config, &work->rpc, &work->call,
                               operation, false, (const uint8_t*)body, length,
                               &response_length);
  h2_alipay_client_json_free((void*)session->config.mem, body);
  yyjson_mut_doc_free(document);
  if (rc != H2_PAL_OK) return rc;
  if (response_length == 0u) return H2_PAL_ERR_NOT_FOUND;
  yyjson_read_err issue;
  work->response = yyjson_read_opts((char*)work->rpc.response, response_length,
                                    0u, &alc, &issue);
  if (work->response == NULL)
    return issue.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(work->response);
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
  if (!yyjson_is_bool(yyjson_obj_get(root, "success")))
    return H2_PAL_ERR_INVALID_STATE;
  return H2_PAL_OK;
}

static yyjson_val* root(union_work_t* work) {
  return yyjson_doc_get_root(work->response);
}

static int finish(h2_alipay_union_t* session, union_work_t* work,
                  h2_alipay_client_payment_t* result, int rc) {
  if (work != NULL) {
    result->requests = work->call.requests;
    result->http_status = work->call.http_status;
    result->rpc_status = work->call.rpc_status;
    yyjson_doc_free(work->response);
    h2_alipay_client_wipe(work, sizeof(*work));
    h2_pal_mem_free(session->config.mem, work);
  }
  result->result = rc;
  return rc;
}

static union_work_t* allocate_work(h2_alipay_union_t* session,
                                   h2_alipay_client_payment_t* result) {
  memset(result, 0, sizeof(*result));
  result->rpc_status = -1;
  union_work_t* work = h2_pal_mem_alloc(session->config.mem, sizeof(*work));
  if (work == NULL) return NULL;
  memset(work, 0, sizeof(*work));
  work->call.rpc_status = -1;
  return work;
}

static int account(h2_alipay_union_t* session, union_work_t* work,
                   h2_alipay_client_payment_t* result) {
  result->stage = H2_ALIPAY_CLIENT_STORAGE;
  binding_record_t binding = {0};
  int rc = h2_alipay_client_load_binding(&session->config, &binding);
  if (rc == H2_PAL_OK && !h2_alipay_client_binding_complete(&binding))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && strlen(binding.uid) > 32u) rc = H2_PAL_ERR_NO_SPACE;
  uint8_t fingerprint[32] = {0};
  if (rc == H2_PAL_OK) {
    h2_sha256_t hash;
    h2_sha256_init(&hash);
    h2_sha256_update(&hash, (const uint8_t*)binding.uid,
                     strlen(binding.uid) + 1u);
    h2_sha256_update(&hash, (const uint8_t*)binding.tid,
                     strlen(binding.tid) + 1u);
    h2_sha256_update(&hash, binding.secret, sizeof(binding.secret));
    h2_sha256_finish(&hash, fingerprint);
    if (session->kind != UNION_NONE &&
        memcmp(fingerprint, session->account_fingerprint,
               sizeof(fingerprint)) != 0) {
      session->kind = UNION_NONE;
      rc = H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (rc == H2_PAL_OK) {
    memcpy(session->account_fingerprint, fingerprint, sizeof(fingerprint));
    rc = copy_text(session->uid, sizeof(session->uid), binding.uid);
    if (rc == H2_PAL_OK)
      rc = copy_text(session->tid, sizeof(session->tid), binding.tid);
    if (rc == H2_PAL_OK)
      rc = h2_alipay_client_load_state(&session->config, &work->rpc.state);
  }
  h2_alipay_client_wipe(fingerprint, sizeof(fingerprint));
  h2_alipay_client_wipe(&binding, sizeof(binding));
  return rc;
}

h2_pal_result_t h2_alipay_union_open(const h2_alipay_client_config_t* config,
                                     h2_alipay_union_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_union_t* session = h2_pal_mem_alloc(config->mem, sizeof(*session));
  if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(session, 0, sizeof(*session));
  session->config = *config;
  *out = session;
  return H2_PAL_OK;
}

void h2_alipay_union_close(h2_alipay_union_t* session) {
  if (session == NULL) return;
  const h2_pal_mem_api_t* memory = session->config.mem;
  h2_alipay_client_wipe(session, sizeof(*session));
  h2_pal_mem_free(memory, session);
}

static int same_user(h2_alipay_union_t* session, union_work_t* work,
                     h2_alipay_scan_order_t* order, bool* parent) {
  const union_field_t fields[] = {{"userId", session->uid},
                                  {"qrCode", session->code}};
  int rc = post(session, work,
                "alipay.mobile.alideviceinfo.pocketmoney.checkPaySameUser",
                fields, 2u, false, false);
  *parent = false;
  if (rc != H2_PAL_OK) return rc;
  yyjson_val* object = root(work);
  if (!yyjson_is_true(yyjson_obj_get(object, "success")))
    return H2_PAL_ERR_UNSUPPORTED;
  yyjson_val* same = yyjson_obj_get(object, "sameUser");
  if (!yyjson_is_true(same)) return H2_PAL_ERR_UNSUPPORTED;
  rc = field(object, "nickName", order->name, sizeof(order->name), true);
  if (rc == H2_PAL_OK)
    rc = field(object, "avatar", order->logo, sizeof(order->logo), false);
  if (rc == H2_PAL_OK) {
    session->kind = UNION_PARENT;
    *parent = true;
  }
  return rc;
}

static int consult(h2_alipay_union_t* session, union_work_t* work,
                   h2_alipay_scan_order_t* order) {
  union_field_t fields[6] = {{"userid", session->uid}, {"tid", session->tid}};
  size_t count = 2u;
  const char* operation = "alipay.mobile.alideviceinfo.iotpay.scancode.consult";
  if (session->kind == UNION_PROXY) {
    operation = "alipay.mobile.payinnovation.standardproxy.consult";
    fields[0] = (union_field_t){"qrCode", session->code};
    fields[1] = (union_field_t){"userId", session->uid};
    fields[count++] = (union_field_t){"tid", session->tid};
    fields[count++] = (union_field_t){"ua", "QZ_watch"};
  } else {
    fields[count++] = (union_field_t){"bizType", session->kind == UNION_TRANSFER
                                                     ? "TransferCode"
                                                     : "UnionCode"};
    if (session->kind == UNION_TRANSFER)
      fields[count++] = (union_field_t){"receiverUid", session->receiver};
    fields[count++] = (union_field_t){"qrCode", session->code};
  }
  int rc = post(session, work, operation, fields, count, false, false);
  if (rc != H2_PAL_OK) return rc;
  yyjson_val* object = root(work);
  if (!yyjson_is_true(yyjson_obj_get(object, "success"))) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  bool transfer = session->kind == UNION_TRANSFER;
  rc = field(object, transfer ? "nickName" : "merchantName", order->name,
             sizeof(order->name), true);
  if (rc == H2_PAL_OK)
    rc = field(object, transfer ? "avatar" : "merchantLogo", order->logo,
               sizeof(order->logo), false);
  if (rc == H2_PAL_OK && session->kind == UNION_PROXY && order->logo[0] == '\0')
    rc = copy_text(order->logo, sizeof(order->logo),
                   "https://mdn.alipayobjects.com/huamei_dzogcs/afts/img/"
                   "A*wBzgTojWk_IAAAAAAAAAAAAADnF7AQ/original");
  if (rc == H2_PAL_OK && session->kind == UNION_PROXY)
    rc = field(object, "amount", order->amount, sizeof(order->amount), false);
  if (rc == H2_PAL_OK && !transfer)
    rc = field(object, "alipayAppId", session->app, sizeof(session->app), true);
  if (rc == H2_PAL_OK && session->kind == UNION_PROXY)
    rc = field(object, "outTradeNo", session->trade, sizeof(session->trade),
               false);
  if (rc == H2_PAL_OK) {
    order->success = true;
    memcpy(order->error, "0", 2u);
  }
  return rc;
}

static int route_uri(h2_alipay_union_t* session, union_work_t* work,
                     h2_alipay_scan_order_t* order) {
  yyjson_val* routes = yyjson_obj_get(root(work), "routeInfos");
  if (!yyjson_is_str(routes)) return H2_PAL_ERR_FORMAT;
  yyjson_alc alc = allocator(session);
  yyjson_doc* document = yyjson_read_opts(
      (char*)yyjson_get_str(routes), yyjson_get_len(routes), 0u, &alc, NULL);
  if (document == NULL) return H2_PAL_ERR_FORMAT;
  char* uri = work->uri;
  int rc = field(yyjson_arr_get_first(yyjson_doc_get_root(document)), "uri",
                 uri, sizeof(work->uri), true);
  yyjson_doc_free(document);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_alipay_client_url_parameter(uri, "appId", session->app,
                                      sizeof(session->app));
  if (rc != H2_PAL_OK) return rc;
  if (strcmp(session->app, "20000123") == 0) {
    bool parent = false;
    rc = same_user(session, work, order, &parent);
    if (rc == H2_PAL_OK && parent) return H2_PAL_ERR_UNSUPPORTED;
    if (rc != H2_PAL_ERR_UNSUPPORTED) return rc;
    char* business = work->decoded;
    rc = h2_alipay_client_url_parameter(uri, "biz_data", business,
                                        sizeof(work->decoded));
    if (rc != H2_PAL_OK) return rc;
    document = yyjson_read_opts(business, strlen(business), 0u, &alc, NULL);
    if (document == NULL) return H2_PAL_ERR_FORMAT;
    rc = field(yyjson_doc_get_root(document), "u", session->receiver,
               sizeof(session->receiver), true);
    yyjson_doc_free(document);
    if (rc != H2_PAL_OK) return rc;
    session->kind = UNION_TRANSFER;
    return consult(session, work, order);
  }
  char* url = work->temporary;
  char source[64], proxy[8];
  rc = h2_alipay_client_url_parameter(uri, "url", url, sizeof(work->temporary));
  if (rc != H2_PAL_OK) return rc;
  rc = h2_alipay_client_url_parameter(url, "source", source, sizeof(source));
  if (rc != H2_PAL_OK) return rc;
  if (strcmp(source, "UnionCode") != 0) return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_alipay_client_url_parameter(url, "ispayProxy", proxy, sizeof(proxy));
  if (rc != H2_PAL_OK || strcmp(proxy, "Y") != 0) return H2_PAL_ERR_UNSUPPORTED;
  rc = h2_alipay_client_url_parameter(url, "qrCode", session->code,
                                      sizeof(session->code));
  if (rc != H2_PAL_OK) return rc;
  session->kind = UNION_AGGREGATE;
  return consult(session, work, order);
}

h2_pal_result_t h2_alipay_union_parse(h2_alipay_union_t* session,
                                      const char* input, bool nfc,
                                      h2_alipay_scan_order_t* order,
                                      h2_alipay_client_payment_t* result) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->rpc_status = -1;
    result->result = H2_PAL_ERR_INVALID_ARG;
  }
  if (session == NULL || input == NULL || order == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  size_t length = 0u;
  if (!text(input, 4096u, &length) || length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_client_config_t config = session->config;
  h2_alipay_client_wipe(session, sizeof(*session));
  session->config = config;
  order->success = false;
  memcpy(order->error, "-1", 3u);
  union_work_t* work = allocate_work(session, result);
  if (work == NULL) return finish(session, NULL, result, H2_PAL_ERR_NO_MEMORY);
  char* scheme = work->temporary;
  char* code = work->decoded;
  const char* parsed = input;
  if (nfc) {
    if (h2_alipay_client_url_parameter(input, "scheme", scheme,
                                       sizeof(work->temporary)) != H2_PAL_OK ||
        h2_alipay_client_url_parameter(scheme, "codeContent", code,
                                       sizeof(work->decoded)) != H2_PAL_OK ||
        code[0] == '\0')
      return finish(session, work, result, H2_PAL_OK);
    char* query = strchr(code, '?');
    if (query != NULL) *query = '\0';
    parsed = code;
  }
  memset(order, 0, sizeof(*order));
  memcpy(order->error, "-1", 3u);
  int rc = copy_text(session->code, sizeof(session->code), parsed);
  if (rc == H2_PAL_OK) rc = account(session, work, result);
  if (rc == H2_PAL_OK) {
    result->stage = H2_ALIPAY_CLIENT_SCAN_ROUTE;
    union_field_t fields[] = {{"channelId", "common"}, {"dataType", ""}};
    rc = post(session, work, "alipay.mobilecodec.route", fields, 2u, true,
              false);
    if (rc == H2_PAL_OK) {
      if (!yyjson_is_true(yyjson_obj_get(root(work), "success"))) {
        if (strncmp(session->code, "https://qr.alipay.com", 21u) == 0) {
          bool parent = false;
          rc = same_user(session, work, order, &parent);
        } else {
          union_field_t model[] = {{"scanCode", session->code},
                                   {"uid", session->uid},
                                   {"tid", session->tid}};
          rc = post(session, work,
                    "alipay.mobile.payinnovation.standardproxy.querypaymodel",
                    model, 3u, false, false);
          if (rc == H2_PAL_OK &&
              yyjson_is_true(yyjson_obj_get(root(work), "success")) &&
              yyjson_get_int(yyjson_obj_get(root(work), "model")) == 101) {
            session->kind = UNION_PROXY;
            result->stage = H2_ALIPAY_CLIENT_SCAN_CONSULT;
            rc = consult(session, work, order);
          } else if (rc == H2_PAL_OK)
            rc = H2_PAL_ERR_UNSUPPORTED;
        }
      } else {
        result->stage = H2_ALIPAY_CLIENT_SCAN_CONSULT;
        rc = route_uri(session, work, order);
      }
    }
  }
  if (rc == H2_PAL_OK && session->kind == UNION_PARENT) {
    order->success = true;
    memcpy(order->error, "0", 2u);
  }
  if (rc == H2_PAL_OK && order->success)
    session->order = *order;
  else
    session->kind = UNION_NONE;
  return finish(session, work, result, rc);
}

static int business_id(h2_alipay_union_t* session) {
  uint64_t milliseconds = 0u;
  int rc = h2_pal_time_get_wall_ms(session->config.time, &milliseconds);
  uint64_t seconds = milliseconds / 1000u;
  if (rc != H2_PAL_OK) return rc;
  if (seconds > UINT32_MAX) return H2_PAL_ERR_INVALID_STATE;
  uint8_t random[4];
  rc = h2_crypto_random(session->config.crypto, random, sizeof(random));
  if (rc != H2_PAL_OK) return rc;
  uint32_t value =
      ((uint32_t)random[0] | ((uint32_t)random[1] << 8u) |
       ((uint32_t)random[2] << 16u) | ((uint32_t)random[3] << 24u)) &
      INT32_MAX;
  int64_t timestamp = seconds <= INT32_MAX
                          ? (int64_t)seconds
                          : (int64_t)seconds - INT64_C(4294967296);
  int length =
      snprintf(session->business, sizeof(session->business), "%s%lld%010lu",
               session->uid, (long long)timestamp, (unsigned long)value);
  return length > 0 && (size_t)length < sizeof(session->business)
             ? H2_PAL_OK
             : H2_PAL_ERR_NO_SPACE;
}

static int failure_code(h2_alipay_union_t* session, yyjson_val* object,
                        h2_alipay_scan_failure_t* failure) {
  yyjson_val* code = yyjson_obj_get(object, "errorCode");
  if (!yyjson_is_str(code)) return H2_PAL_ERR_INVALID_STATE;
  if (yyjson_equals_str(code, "2013")) {
    session->kind = UNION_NONE;
    *failure = H2_ALIPAY_SCAN_ACCOUNT_MISSING;
    return h2_alipay_client_discard_binding(&session->config);
  }
  if (yyjson_equals_str(code, "2084"))
    *failure = H2_ALIPAY_SCAN_BALANCE_INSUFFICIENT;
  else if (yyjson_equals_str(code, "2086"))
    *failure = H2_ALIPAY_SCAN_AMOUNT_LIMIT;
  return H2_PAL_OK;
}

static int query_payment(h2_alipay_union_t* session, union_work_t* work,
                         h2_alipay_scan_failure_t* failure,
                         h2_alipay_client_payment_t* result) {
  union_field_t fields[5] = {
      {"userid", session->uid},
      {"tid", session->tid},
      {"outBizNo", session->business},
      {"bizType",
       session->kind == UNION_TRANSFER ? "TransferCode" : "UnionCode"}};
  fields[4] = session->kind == UNION_TRANSFER
                  ? (union_field_t){"receiverUid", session->receiver}
                  : (union_field_t){"tradeNo", session->trade};
  int rc = post(session, work,
                "alipay.mobile.alideviceinfo.iotpay.scancode.payResult", fields,
                5u, false, false);
  if (rc != H2_PAL_OK) return rc;
  yyjson_val* object = root(work);
  if (!yyjson_is_true(yyjson_obj_get(object, "success")))
    return H2_PAL_ERR_INVALID_STATE;
  yyjson_val* code = yyjson_obj_get(object, "resultCode");
  if (!yyjson_is_str(code)) return H2_PAL_ERR_INVALID_STATE;
  if (yyjson_equals_str(code, "SUCCESS"))
    result->state = H2_ALIPAY_CLIENT_PAYMENT_PAID;
  else if (yyjson_equals_str(code, "FAIL")) {
    result->state = H2_ALIPAY_CLIENT_PAYMENT_FAILED;
    h2_alipay_scan_failure_t query_failure = H2_ALIPAY_SCAN_COMMON_FAILURE;
    rc = failure_code(session, object, &query_failure);
    if (query_failure == H2_ALIPAY_SCAN_ACCOUNT_MISSING) {
      *failure = query_failure;
      if (rc != H2_PAL_OK) return rc;
      result->state = H2_ALIPAY_CLIENT_PAYMENT_UNBOUND;
    }
    return H2_PAL_ERR_INVALID_STATE;
  } else
    return H2_PAL_ERR_INVALID_STATE;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_union_confirm(h2_alipay_union_t* session,
                                        const h2_alipay_scan_order_t* order,
                                        h2_alipay_scan_failure_t* failure,
                                        h2_alipay_client_payment_t* result) {
  if (failure != NULL) *failure = H2_ALIPAY_SCAN_COMMON_FAILURE;
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
    result->rpc_status = -1;
    result->result = H2_PAL_ERR_INVALID_ARG;
  }
  if (session == NULL || order == NULL || failure == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  size_t length = 0u;
  if (session->kind == UNION_NONE ||
      !text(order->amount, sizeof(order->amount), &length) || length == 0u ||
      memcmp(order->name, session->order.name, sizeof(order->name)) != 0 ||
      memcmp(order->logo, session->order.logo, sizeof(order->logo)) != 0 ||
      !order->success ||
      (session->order.amount[0] != '\0' &&
       strcmp(order->amount, session->order.amount) != 0))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = H2_PAL_OK;
  union_work_t* work = allocate_work(session, result);
  if (work == NULL) return finish(session, NULL, result, H2_PAL_ERR_NO_MEMORY);
  rc = account(session, work, result);
  result->stage = H2_ALIPAY_CLIENT_SCAN_PAY;
  if (rc == H2_PAL_OK && session->submitted) {
    rc = session->kind == UNION_PARENT
             ? H2_PAL_ERR_INVALID_STATE
             : query_payment(session, work, failure, result);
  } else if (rc == H2_PAL_OK) {
    const char* operation =
        "alipay.mobile.alideviceinfo.iotpay.scancode.directPay";
    union_field_t fields[10];
    size_t count = 0u;
    if (session->kind == UNION_PARENT) {
      operation = "alipay.mobile.alideviceinfo.pocketmoney.withdrawByWatch";
      fields[count++] = (union_field_t){"parentUid", session->uid};
      fields[count++] = (union_field_t){"tid", session->tid};
      fields[count++] = (union_field_t){"amount", order->amount};
    } else {
      rc = business_id(session);
      bool proxy = session->kind == UNION_PROXY;
      if (proxy)
        operation = "alipay.mobile.payinnovation.standardproxy.directPay";
      fields[count++] =
          (union_field_t){proxy ? "userId" : "userid", session->uid};
      fields[count++] = (union_field_t){"tid", session->tid};
      fields[count++] = (union_field_t){"appId", session->app};
      if (proxy) {
        fields[count++] = (union_field_t){"qrCode", session->code};
        fields[count++] = (union_field_t){"idempotentId", session->business};
      }
      fields[count++] = (union_field_t){"amount", order->amount};
      if (!proxy) fields[count++] = (union_field_t){"qrCode", session->code};
      fields[count++] = (union_field_t){"remark", ""};
      if (proxy) {
        fields[count++] = (union_field_t){"outTradeNo", session->trade};
        fields[count++] = (union_field_t){"ua", "QZ_watch"};
      } else
        fields[count++] = (union_field_t){"outBizNo", session->business};
    }
    unsigned previous = work->call.requests;
    if (rc == H2_PAL_OK)
      rc = post(session, work, operation, fields, count, false,
                session->kind != UNION_PROXY && session->kind != UNION_PARENT);
    if (work->call.requests > previous) {
      session->submitted = true;
      memcpy(session->order.amount, order->amount, length + 1u);
    }
    if (rc == H2_PAL_OK) {
      yyjson_val* object = root(work);
      if (session->kind == UNION_PARENT) {
        result->state = yyjson_is_true(yyjson_obj_get(object, "success"))
                            ? H2_ALIPAY_CLIENT_PAYMENT_PAID
                            : H2_ALIPAY_CLIENT_PAYMENT_FAILED;
        if (!yyjson_is_true(yyjson_obj_get(object, "success")))
          rc = failure_code(session, object, failure);
      } else if (!yyjson_is_true(yyjson_obj_get(object, "success"))) {
        rc = H2_PAL_ERR_INVALID_STATE;
      } else if (session->kind != UNION_TRANSFER) {
        rc = field(object, "tradeNo", session->trade, sizeof(session->trade),
                   true);
        yyjson_val* status = yyjson_obj_get(object, "resultCode");
        if (rc == H2_PAL_OK && yyjson_equals_str(status, "SUCCESS"))
          result->state = H2_ALIPAY_CLIENT_PAYMENT_PAID;
        else if (rc == H2_PAL_OK && yyjson_equals_str(status, "FAIL")) {
          result->state = H2_ALIPAY_CLIENT_PAYMENT_FAILED;
          rc = failure_code(session, object, failure);
        } else if (rc == H2_PAL_OK)
          rc = query_payment(session, work, failure, result);
      } else {
        if (rc == H2_PAL_OK) rc = query_payment(session, work, failure, result);
      }
    }
  }
  return finish(session, work, result, rc);
}
