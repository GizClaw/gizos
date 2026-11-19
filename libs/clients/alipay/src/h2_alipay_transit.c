#include "h2_alipay_transit_internal.h"

#include "h2_alipay_client_internal.h"
#include "h2_encoding.h"
#include "yyjson.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static yyjson_alc transit_allocator(const h2_alipay_client_config_t* config) {
  const yyjson_alc allocator = {h2_alipay_client_json_alloc,
                                h2_alipay_client_json_realloc,
                                h2_alipay_client_json_free, (void*)config->mem};
  return allocator;
}

static yyjson_val* member(yyjson_val* object, const char* name) {
  return h2_alipay_client_json_member(object, name);
}

static const char* text(yyjson_val* object, const char* name) {
  return yyjson_get_str(member(object, name));
}

static int32_t json_integer(yyjson_val* value) {
  double number = yyjson_get_num(value);
  if (number >= INT32_MAX) return INT32_MAX;
  if (number <= INT32_MIN) return INT32_MIN;
  return yyjson_is_true(value) ? 1 : (int32_t)number;
}

static uint32_t decimal(const char* input) {
  if (input == NULL) return 0u;
  while (*input == ' ' || (*input >= '\t' && *input <= '\r')) ++input;
  bool negative = *input == '-';
  if (*input == '-' || *input == '+') ++input;
  uint32_t value = 0u;
  while (*input >= '0' && *input <= '9')
    value = value * 10u + (uint32_t)(*input++ - '0');
  return negative ? 0u - value : value;
}

static int now(const h2_alipay_client_config_t* config, uint32_t* output) {
  uint64_t milliseconds = 0u;
  int rc = h2_pal_time_get_wall_ms(config->time, &milliseconds);
  if (rc == H2_PAL_OK) *output = (uint32_t)(milliseconds / 1000u);
  return rc;
}

static int get_blob(const h2_alipay_client_config_t* config, const char* key,
                    void** output, size_t* length) {
  *output = NULL;
  *length = 0u;
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = namespace->get_blob != NULL
           ? namespace->get_blob(namespace, config->mem, key, output, length)
           : H2_PAL_ERR_UNSUPPORTED;
  return h2_alipay_client_finish_namespace(namespace, rc);
}

static int set_blob(const h2_alipay_client_config_t* config, const char* key,
                    const void* input, size_t length) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = namespace->set_blob != NULL && namespace->commit != NULL
           ? namespace->set_blob(namespace, key, input, length)
           : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  return h2_alipay_client_finish_namespace(namespace, rc);
}

static int setting(const h2_alipay_client_config_t* config, const char* key,
                   bool write, uint32_t* value) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(
      config->pref, config->pref_namespace,
      write ? H2_PAL_PREF_OPEN_READ_WRITE : H2_PAL_PREF_OPEN_READ_ONLY,
      &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (write) {
    rc = namespace->set_u32 != NULL && namespace->commit != NULL
             ? namespace->set_u32(namespace, key, *value)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  } else {
    rc = namespace->get_u32 != NULL ? namespace->get_u32(namespace, key, value)
                                    : H2_PAL_ERR_UNSUPPORTED;
  }
  return h2_alipay_client_finish_namespace(namespace, rc);
}

static int read_json(const h2_alipay_client_config_t* config, const char* key,
                     yyjson_doc** output) {
  *output = NULL;
  void* raw = NULL;
  size_t length = 0u;
  int rc = get_blob(config, key, &raw, &length);
  if (rc == H2_PAL_OK) {
    yyjson_alc allocator = transit_allocator(config);
    yyjson_read_err error;
    *output = yyjson_read_opts(raw, length, YYJSON_READ_STOP_WHEN_DONE,
                               &allocator, &error);
    if (*output == NULL)
      rc = error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  }
  h2_alipay_client_wipe(raw, length);
  h2_pal_mem_free(config->mem, raw);
  return rc;
}

static int write_json(const h2_alipay_client_config_t* config, const char* key,
                      yyjson_mut_doc* document) {
  yyjson_alc allocator = transit_allocator(config);
  size_t length = 0u;
  char* encoded =
      yyjson_mut_write_opts(document, 0u, &allocator, &length, NULL);
  if (encoded == NULL) return H2_PAL_ERR_NO_MEMORY;
  int rc = set_blob(config, key, encoded, length);
  h2_alipay_client_wipe(encoded, length);
  allocator.free(allocator.ctx, encoded);
  return rc;
}

static int rpc(const h2_alipay_client_config_t* config, const char* operation,
               const char* number, const char* type, yyjson_val* script,
               yyjson_doc** output) {
  *output = NULL;
  client_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  binding_record_t* binding = h2_pal_mem_alloc(config->mem, sizeof(*binding));
  if (work == NULL || binding == NULL) {
    h2_pal_mem_free(config->mem, work);
    h2_pal_mem_free(config->mem, binding);
    return 29;
  }
  memset(work, 0, sizeof(*work));
  memset(binding, 0, sizeof(*binding));
  yyjson_alc allocator = transit_allocator(config);
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  yyjson_mut_val* array = document != NULL ? yyjson_mut_arr(document) : NULL;
  yyjson_mut_val* object = document != NULL ? yyjson_mut_obj(document) : NULL;
  char* body = NULL;
  int result = 29;
  int rc = h2_alipay_client_load_binding(config, binding);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) goto done;
  if (array == NULL || object == NULL ||
      !yyjson_mut_arr_append(array, object) ||
      !yyjson_mut_obj_add_str(document, object, "source",
                              config->product_type < 3u ? "ADULT" : "CHILD") ||
      !yyjson_mut_obj_add_str(document, object, "userId", binding->uid) ||
      !yyjson_mut_obj_add_str(document, object, "tid", binding->tid) ||
      (config->product_type >= 3u &&
       !yyjson_mut_obj_add_str(document, object, "appId", "30000019")))
    goto done;
  if (number != NULL) {
    if (!yyjson_mut_obj_add_str(document, object, "cardNo", number) ||
        !yyjson_mut_obj_add_str(document, object, "cardType", type))
      goto done;
    if (script == NULL) {
      if (!yyjson_mut_obj_add_str(document, object, "codeSDKVersion",
                                  "0.0.8") ||
          !yyjson_mut_obj_add_uint(document, object, "watchSDKVersion", 1u))
        goto done;
    } else if (text(script, "scriptType") == NULL ||
               text(script, "scriptName") == NULL ||
               text(script, "mac") == NULL ||
               !yyjson_mut_obj_add_str(document, object, "scriptType",
                                       text(script, "scriptType")) ||
               !yyjson_mut_obj_add_str(document, object, "scriptName",
                                       text(script, "scriptName")) ||
               !yyjson_mut_obj_add_str(document, object, "mac",
                                       text(script, "mac"))) {
      goto done;
    }
  }
  yyjson_mut_doc_set_root(document, array);
  size_t length = 0u;
  body = yyjson_mut_write_opts(document, 0u, &allocator, &length, NULL);
  if (body == NULL) goto done;
  h2_alipay_client_result_t diagnostics = {0};
  rc = h2_alipay_client_load_state(config, &work->state);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_client_post(config, work, &diagnostics, operation, false,
                               (const uint8_t*)body, length, &length);
  if (rc != H2_PAL_OK) {
    result = rc == H2_PAL_ERR_NO_MEMORY ? 29 : 7;
  } else {
    yyjson_read_err error;
    *output = yyjson_read_opts((char*)work->response, length,
                               YYJSON_READ_STOP_WHEN_DONE, &allocator, &error);
    result = *output == NULL ? 29 : 0;
  }
done:
  if (body != NULL) allocator.free(allocator.ctx, body);
  yyjson_mut_doc_free(document);
  h2_alipay_client_wipe(binding, sizeof(*binding));
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, binding);
  h2_pal_mem_free(config->mem, work);
  return result;
}

int h2_alipay_transit_activity(const h2_alipay_client_config_t* config,
                               bool online, bool* active) {
  if (config == NULL || active == NULL) return 19;
  uint32_t value = 0u;
  if (!online) {
    int rc = setting(config, "transit_active", false, &value);
    *active = rc == H2_PAL_OK && value == 165u;
    return rc == H2_PAL_OK ? 0 : 2;
  }
  yyjson_doc* document = NULL;
  int result = rpc(config, "com.alideviceinfo.transport.query.queryStatus",
                   NULL, NULL, NULL, &document);
  if (result == 0) {
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* success = member(root, "success");
    yyjson_val* open = member(root, "open");
    if (success == NULL || (yyjson_is_true(success) && open == NULL)) {
      result = 7;
    } else if (!yyjson_is_true(success)) {
      result = 26;
    } else {
      value = yyjson_is_true(open) ? 165u : 0u;
      result =
          setting(config, "transit_active", true, &value) == H2_PAL_OK ? 0 : 1;
      if (result == 0) *active = value == 165u;
    }
  }
  yyjson_doc_free(document);
  return result;
}

static bool prefix(const char* value, const char* expected) {
  return value != NULL && strlen(value) <= strlen(expected) &&
         memcmp(value, expected, strlen(value)) == 0;
}

static void copy_text(uint8_t* output, const char* input, bool title) {
  size_t length = strlen(input);
  if (title && length > 39u) {
    length = 36u;
    while (length != 0u && ((uint8_t)input[length] & 0xc0u) == 0x80u) --length;
    memcpy(output, input, length);
    memcpy(output + length, "...", 4u);
  } else {
    if (length > 39u) length = 39u;
    memcpy(output, input, length);
    output[length] = 0u;
  }
}

int h2_alipay_transit_cards(const h2_alipay_client_config_t* config,
                            h2_alipay_transit_t* state, bool online,
                            uint8_t* output, size_t* length, uint32_t* count) {
  if (state == NULL) return 19;
  state->card_result = 99;
  if (config == NULL || output == NULL || length == NULL || count == NULL) {
    state->card_result = 4;
    return 19;
  }
  if (!online) {
    uint32_t stored_count = 0u;
    int rc = setting(config, "transit_count", false, &stored_count);
    if (rc != H2_PAL_OK || stored_count > UINT32_MAX / 120u) {
      state->card_result = 4;
      return 5;
    }
    *count = stored_count;
    size_t required = (size_t)stored_count * 120u;
    if (*length < required) {
      *length = required;
      state->card_result = 5;
      return 15;
    }
    void* raw = NULL;
    size_t size = 0u;
    rc = get_blob(config, "transit_cards", &raw, &size);
    if (rc == H2_PAL_OK && (size != (required != 0u ? required : 4u) ||
                            size > *length || raw == NULL))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) {
      memcpy(output, raw, size);
      if (required != 0u) *length = size;
    }
    h2_pal_mem_free(config->mem, raw);
    if (rc != H2_PAL_OK) state->card_result = 4;
    return rc == H2_PAL_OK ? 0 : 5;
  }
  yyjson_doc* document = NULL;
  int result = rpc(config, "com.alideviceinfo.transport.query.queryAllCards",
                   NULL, NULL, NULL, &document);
  if (result != 0) {
    state->card_result = 1;
    return result;
  }
  yyjson_val* root = yyjson_doc_get_root(document);
  yyjson_val* success = member(root, "success");
  yyjson_val* cards = member(root, "cardList");
  if (!yyjson_is_true(success)) {
    const char* error = text(root, "errorCode");
    state->card_result = prefix(error, "2099")                    ? 0
                         : prefix(error, "CARD_TYPE_NOT_SUPPORT") ? 2
                                                                  : 1;
    result = success == NULL ? 7 : 26;
    goto done;
  }
  if (cards == NULL) {
    state->card_result = 1;
    result = 7;
    goto done;
  }
  size_t size = yyjson_is_arr(cards) ? yyjson_arr_size(cards) : 0u;
  if (size > UINT32_MAX / 120u) {
    state->card_result = 4;
    result = 19;
    goto done;
  }
  size_t required = size * 120u;
  if (*length < required) {
    *count = (uint32_t)size;
    *length = required;
    state->card_result = 5;
    result = 15;
    goto done;
  }
  for (size_t index = 0u; index < size; ++index) {
    yyjson_val* card = yyjson_arr_get(cards, index);
    if (text(card, "cardNo") == NULL || text(card, "cardType") == NULL ||
        text(card, "title") == NULL) {
      state->card_result = 4;
      result = 7;
      goto done;
    }
  }
  for (size_t index = 0u; index < size; ++index) {
    yyjson_val* card = yyjson_arr_get(cards, index);
    copy_text(output + index * 120u, text(card, "cardNo"), false);
    copy_text(output + index * 120u + 40u, text(card, "cardType"), false);
    copy_text(output + index * 120u + 80u, text(card, "title"), true);
  }
  *count = (uint32_t)size;
  if (required != 0u)
    *length = required;
  else if (*length >= 4u)
    memset(output, 0, 4u);
  else {
    state->card_result = 5;
    result = 15;
    goto done;
  }
  uint32_t stored_count = (uint32_t)size;
  int rc = setting(config, "transit_count", true, &stored_count);
  if (rc == H2_PAL_OK)
    rc = set_blob(config, "transit_cards", output,
                  required != 0u ? required : 4u);
  if (rc != H2_PAL_OK) {
    state->card_result = 4;
    result = 5;
  }
done:
  yyjson_doc_free(document);
  return result;
}

static int data_error(yyjson_val* root, h2_alipay_transit_t* state) {
  const char* error = text(root, "errorCode");
  if (error == NULL) {
    state->code_result = 4;
    return 7;
  }
  if (prefix(error, "2099")) {
    state->code_result = 0;
    return 57;
  }
  static const struct {
    const char* value;
    int detail, result;
  } errors[] = {
      {"MONTH_TICKET_NOT_BUY", 6, 58},
      {"BALANCE_THRESHOLD_LIMIT", 6, 58},
      {"BALANCE_NOT_ENOUGH", 6, 58},
      {"BUY_DAY_CARD", 15, 54},
      {"NO_RECHARGE_BALANCE", 16, 58},
      {"MONTH_CARD_ARREARS", 16, 58},
      {"METRO_ABNORMAL_ORDER", 7, 54},
      {"METRO_SUPPLEMENT_ORDER", 7, 54},
      {"CARD_OVERDUE", 7, 54},
      {"CARD_REVOKING", 8, 54},
      {"MERCHANT_STATUS_OFF", 8, 54},
      {"ACCOUNT_CANCELED", 8, 54},
      {"CARD_STATUS_NOT_ON", 8, 54},
      {"SENIOR_CARD_HAS_BEEN_ACTIVATED", 9, 54},
      {"MONTH_TICKET_REFUNDED", 10, 54},
      {"MONTH_TICKET_REFUNDING", 10, 54},
      {"USER_INSURE_EXPIRE", 11, 54},
      {"GO_ANNUAL_REVIEW", 12, 54},
      {"GO_CHECK", 13, 54},
  };
  for (size_t index = 0u; index < sizeof(errors) / sizeof(*errors); ++index) {
    if (strcmp(error, errors[index].value) == 0) {
      state->code_result = errors[index].detail;
      return errors[index].result;
    }
  }
  state->code_result = 14;
  return 56;
}

typedef struct transit_usage {
  uint32_t mg_limit, og_limit, mg_used, og_used, mg_checked, og_checked;
} transit_usage_t;

static int usage_load(const h2_alipay_client_config_t* config,
                      yyjson_val* record, transit_usage_t* usage) {
  memset(usage, 0, sizeof(*usage));
  yyjson_val* render = member(record, "render");
  const char* encoded = text(render, "config");
  if (encoded == NULL) return H2_PAL_ERR_FORMAT;
  yyjson_alc allocator = transit_allocator(config);
  yyjson_read_err error;
  yyjson_doc* document =
      yyjson_read_opts((char*)encoded, strlen(encoded),
                       YYJSON_READ_STOP_WHEN_DONE, &allocator, &error);
  if (document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* values = yyjson_doc_get_root(document);
  usage->mg_limit = decimal(text(values, "QR_MG_TIM"));
  usage->og_limit = decimal(text(values, "QR_OG_TIM"));
  usage->mg_used = (uint32_t)yyjson_get_uint(member(record, "mg_used"));
  usage->og_used = (uint32_t)yyjson_get_uint(member(record, "og_used"));
  usage->mg_checked = (uint32_t)yyjson_get_uint(member(record, "mg_checked"));
  usage->og_checked = (uint32_t)yyjson_get_uint(member(record, "og_checked"));
  yyjson_doc_free(document);
  return H2_PAL_OK;
}

static bool usage_expired(uint32_t checked, uint32_t timestamp) {
  return checked == 0u || (uint64_t)checked + 43200u <= timestamp;
}

static int store_data(const h2_alipay_client_config_t* config,
                      const char* number, const char* type, yyjson_val* render,
                      const transit_usage_t* usage) {
  yyjson_doc* previous = NULL;
  int rc = read_json(config, "transit_data", &previous);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) return rc;
  yyjson_alc allocator = transit_allocator(config);
  yyjson_mut_doc* document = previous != NULL
                                 ? yyjson_doc_mut_copy(previous, &allocator)
                                 : yyjson_mut_doc_new(&allocator);
  yyjson_mut_val* root = previous != NULL   ? yyjson_mut_doc_get_root(document)
                         : document != NULL ? yyjson_mut_obj(document)
                                            : NULL;
  yyjson_mut_val* record = document != NULL ? yyjson_mut_obj(document) : NULL;
  yyjson_mut_val* data =
      document != NULL ? yyjson_val_mut_copy(document, render) : NULL;
  uint32_t timestamp = 0u;
  rc = now(config, &timestamp);
  if (rc == H2_PAL_OK &&
      (root == NULL || !yyjson_mut_is_obj(root) || record == NULL ||
       data == NULL ||
       !yyjson_mut_obj_add_strcpy(document, record, "number", number) ||
       !yyjson_mut_obj_add_uint(document, record, "updated", timestamp) ||
       !yyjson_mut_obj_add_uint(document, record, "mg_used",
                                usage != NULL ? usage->mg_used : 0u) ||
       !yyjson_mut_obj_add_uint(document, record, "og_used",
                                usage != NULL ? usage->og_used : 0u) ||
       !yyjson_mut_obj_add_uint(
           document, record, "mg_checked",
           usage != NULL ? usage->mg_checked : timestamp) ||
       !yyjson_mut_obj_add_uint(
           document, record, "og_checked",
           usage != NULL ? usage->og_checked : timestamp) ||
       !yyjson_mut_obj_add_val(document, record, "render", data)))
    rc = H2_PAL_ERR_NO_MEMORY;
  if (rc == H2_PAL_OK) {
    yyjson_mut_doc_set_root(document, root);
    (void)yyjson_mut_obj_remove_str(root, type);
    yyjson_mut_val* key = yyjson_mut_strcpy(document, type);
    if (key == NULL || !yyjson_mut_obj_add(root, key, record))
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  if (rc == H2_PAL_OK) rc = write_json(config, "transit_data", document);
  yyjson_mut_doc_free(document);
  yyjson_doc_free(previous);
  return rc;
}

static int transit_update_data(const h2_alipay_client_config_t* config,
                               h2_alipay_transit_t* state, const char* number,
                               const char* type) {
  if (state == NULL) return 99;
  state->code_result = 99;
  if (config == NULL || number == NULL || type == NULL) {
    state->code_result = 5;
    return 99;
  }
  yyjson_doc* document = NULL;
  int result = rpc(config, "com.alideviceinfo.transport.query.queryCardData",
                   number, type, NULL, &document);
  if (result != 0) {
    state->code_result = 1;
    return result;
  }
  yyjson_val* root = yyjson_doc_get_root(document);
  if (member(root, "success") == NULL) {
    state->code_result = 1;
    result = 7;
  } else if (!yyjson_is_true(member(root, "success"))) {
    result = data_error(root, state);
  } else {
    yyjson_val* render = member(root, "renderData");
    const char* mode = text(render, "renderType");
    const char* config_text = text(render, "config");
    if (mode == NULL || strlen(mode) > 31u || config_text == NULL ||
        member(render, "expiredTime") == NULL ||
        text(render, "certServiceType") == NULL ||
        text(render, "authMode") == NULL) {
      state->code_result = 1;
      result = 7;
    } else {
      int rc = store_data(config, number, type, render, NULL);
      result = rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_MEMORY ? 29 : 5;
    }
  }
  yyjson_doc_free(document);
  return result;
}

int h2_alipay_transit_status(const h2_alipay_client_config_t* config,
                             const char* type, bool* exists, int32_t* expiry,
                             int32_t* remaining) {
  if (config == NULL || type == NULL || exists == NULL || expiry == NULL ||
      remaining == NULL)
    return 19;
  uint32_t timestamp = 0u;
  if (now(config, &timestamp) != H2_PAL_OK) return 99;
  *exists = false;
  *expiry = timestamp <= INT32_MAX ? (int32_t)timestamp
                                   : (int32_t)((int64_t)timestamp - 4294967296);
  *remaining = 0;
  yyjson_doc* document = NULL;
  int rc = read_json(config, "transit_data", &document);
  yyjson_val* record = yyjson_obj_get(yyjson_doc_get_root(document), type);
  yyjson_val* render = member(record, "render");
  if (rc == H2_PAL_OK && render != NULL) {
    *exists = true;
    *expiry = json_integer(member(render, "expiredTime"));
    if ((int64_t)*expiry >= (int64_t)(int32_t)timestamp) {
      transit_usage_t usage;
      int loaded = usage_load(config, record, &usage);
      if (loaded == H2_PAL_OK) {
        uint32_t og_used =
            usage_expired(usage.og_checked, timestamp) ? 0u : usage.og_used;
        uint32_t mg_used =
            usage_expired(usage.mg_checked, timestamp) ? 0u : usage.mg_used;
        uint32_t offline =
            og_used <= usage.og_limit ? usage.og_limit - og_used : 0u;
        uint32_t total =
            mg_used <= usage.mg_limit ? usage.mg_limit - mg_used : 0u;
        uint32_t available = offline < total ? offline : total;
        *remaining = available <= INT32_MAX ? (int32_t)available : INT32_MAX;
      } else if (loaded == H2_PAL_ERR_NO_MEMORY) {
        rc = loaded;
      }
    }
  }
  yyjson_doc_free(document);
  return rc == H2_PAL_ERR_NO_MEMORY ? 29 : 0;
}

static int yct_generate(const h2_alipay_client_config_t* config,
                        yyjson_val* render, uint32_t timestamp, uint8_t* output,
                        size_t* length) {
  const char* certificate = text(render, "offlineCert");
  const char* private_text = text(render, "certPrivateKey");
  if (certificate == NULL || private_text == NULL) return 43;
  yyjson_alc allocator = transit_allocator(config);
  yyjson_doc* document =
      yyjson_read_opts((char*)certificate, strlen(certificate),
                       YYJSON_READ_STOP_WHEN_DONE, &allocator, NULL);
  yyjson_doc* private_document =
      yyjson_read_opts((char*)private_text, strlen(private_text),
                       YYJSON_READ_STOP_WHEN_DONE, &allocator, NULL);
  yyjson_val* root = yyjson_doc_get_root(document);
  yyjson_val* keys = yyjson_doc_get_root(private_document);
  const char* pay_type = text(root, "pay_type");
  const char* start = text(root, "cert_start_time");
  const char* data = text(root, "cert_data");
  const char* account_text = text(keys, "account_issue_sub_key");
  const char* terminal_text = text(keys, "terminal_authen_sub_key");
  uint8_t account_key[16] = {0}, terminal_key[16] = {0};
  uint8_t message[64] = {0}, packet[384] = {0};
  uint8_t account_mac[8] = {0}, terminal_mac[8] = {0}, type[16] = {0};
  char encoded[512];
  size_t size = 0u, message_length = 0u, type_length = 0u, packet_length = 0u;
  size_t encoded_length = 0u;
  int rc = H2_PAL_ERR_FORMAT;
  if (pay_type == NULL || start == NULL || data == NULL ||
      account_text == NULL || terminal_text == NULL ||
      strlen(account_text) != 32u || strlen(terminal_text) != 32u ||
      strlen(start) > 32u || strlen(pay_type) > 32u)
    goto done;
  rc = h2_encoding_decode(&h2_encoding_hex, account_text, 32u, account_key,
                          sizeof(account_key), &size);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, terminal_text, 32u, terminal_key,
                            sizeof(terminal_key), &size);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, start, strlen(start), message,
                            16u, &message_length);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, pay_type, strlen(pay_type), type,
                            sizeof(type), &type_length);
  uint8_t timestamp_bytes[4] = {(uint8_t)(timestamp >> 24u),
                                (uint8_t)(timestamp >> 16u),
                                (uint8_t)(timestamp >> 8u), (uint8_t)timestamp};
  if (rc == H2_PAL_OK) {
    memcpy(message + message_length, timestamp_bytes, 4u);
    message_length += 4u;
    memcpy(message + message_length, type, type_length);
    message_length += type_length;
    rc = h2_crypto_iso9797_alg3_mac(config->crypto, account_key, message,
                                    message_length, account_mac);
  }
  if (rc == H2_PAL_OK) {
    memcpy(message + message_length, account_mac, 4u);
    message_length += 4u;
    rc = h2_crypto_iso9797_alg3_mac(config->crypto, terminal_key, message,
                                    message_length, terminal_mac);
  }
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, data, strlen(data), packet,
                            sizeof(packet) - type_length - 12u, &packet_length);
  if (rc == H2_PAL_OK) {
    memcpy(packet + packet_length, type, type_length);
    packet_length += type_length;
    memcpy(packet + packet_length, timestamp_bytes, 4u);
    packet_length += 4u;
    memcpy(packet + packet_length, terminal_mac, 4u);
    packet_length += 4u;
    memcpy(packet + packet_length, account_mac, 4u);
    packet_length += 4u;
    rc = h2_encoding_encode(&h2_encoding_base64_std, packet, packet_length,
                            encoded, sizeof(encoded), &encoded_length);
  }
  if (rc == H2_PAL_OK) {
    if (*length < encoded_length + 2u)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      memcpy(output, "KT", 2u);
      memcpy(output + 2u, encoded, encoded_length);
      *length = encoded_length + 2u;
    }
  }
done:
  h2_alipay_client_wipe(account_key, sizeof(account_key));
  h2_alipay_client_wipe(terminal_key, sizeof(terminal_key));
  h2_alipay_client_wipe(message, sizeof(message));
  h2_alipay_client_wipe(packet, sizeof(packet));
  h2_alipay_client_wipe(account_mac, sizeof(account_mac));
  h2_alipay_client_wipe(terminal_mac, sizeof(terminal_mac));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  yyjson_doc_free(document);
  yyjson_doc_free(private_document);
  return rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_SPACE ? 15 : 43;
}

static int append_hex(const char* input, uint8_t* output, size_t capacity,
                      size_t* length) {
  if (input == NULL || *length > capacity) return H2_PAL_ERR_FORMAT;
  size_t size = 0u;
  int rc = h2_encoding_decode(&h2_encoding_hex, input, strlen(input),
                              output + *length, capacity - *length, &size);
  if (rc == H2_PAL_OK) *length += size;
  return rc;
}

static int rct_generate(const h2_alipay_client_config_t* config,
                        yyjson_val* render, uint32_t timestamp, uint8_t* output,
                        size_t* length) {
  const char* certificate = text(render, "offlineCert");
  const char* private_text = text(render, "certPrivateKey");
  if (certificate == NULL || private_text == NULL ||
      strlen(private_text) != 64u)
    return 43;
  yyjson_alc allocator = transit_allocator(config);
  yyjson_doc* document =
      yyjson_read_opts((char*)certificate, strlen(certificate),
                       YYJSON_READ_STOP_WHEN_DONE, &allocator, NULL);
  yyjson_val* root = yyjson_doc_get_root(document);
  const char* data = text(root, "cert_data");
  const char* platform = text(root, "platform_sign");
  const char* merchant = text(root, "merchant_user_id");
  const char* limit = text(root, "amount_limit");
  const char* business = text(root, "other_biz_info");
  uint8_t message[384] = {0}, packet[384] = {0}, nonce[16] = {0};
  char material[100] = {0}, encoded[512];
  h2_pal_sm2_private_key_t private_key = {{0}};
  h2_pal_sm2_signature_t signature = {{0}};
  size_t size = 0u, message_length = 0u, packet_length = 1u;
  size_t encoded_length = 0u;
  int rc = H2_PAL_ERR_FORMAT;
  if (data == NULL || platform == NULL || merchant == NULL || limit == NULL ||
      business == NULL)
    goto done;
  int formatted =
      snprintf(material, sizeof(material), "%s%s%08X", config->identity.mac,
               config->identity.model, (unsigned)timestamp);
  if (formatted < 0) goto done;
  rc = h2_pal_crypto_md5(config->crypto, (const uint8_t*)material,
                         strlen(material), nonce);
  if (rc == H2_PAL_OK)
    rc = append_hex(platform, message, sizeof(message) - 24u, &message_length);
  uint32_t seconds = decimal(text(root, "qrcode_valid_seconds"));
  if (seconds == 0u || seconds > INT32_MAX) seconds = 300u;
  uint32_t relative = timestamp - 1483200000u;
  uint32_t expires = relative + seconds;
  if (rc == H2_PAL_OK) {
    memcpy(message + message_length, nonce, sizeof(nonce));
    message_length += sizeof(nonce);
    message[message_length++] = (uint8_t)(relative >> 24u);
    message[message_length++] = (uint8_t)(relative >> 16u);
    message[message_length++] = (uint8_t)(relative >> 8u);
    message[message_length++] = (uint8_t)relative;
    message[message_length++] = (uint8_t)(expires >> 24u);
    message[message_length++] = (uint8_t)(expires >> 16u);
    message[message_length++] = (uint8_t)(expires >> 8u);
    message[message_length++] = (uint8_t)expires;
    rc = append_hex(merchant, message, sizeof(message), &message_length);
  }
  if (rc == H2_PAL_OK)
    rc = append_hex(limit, message, sizeof(message), &message_length);
  if (rc == H2_PAL_OK)
    rc = append_hex(business, message, sizeof(message), &message_length);
  if (rc == H2_PAL_OK)
    rc =
        h2_encoding_decode(&h2_encoding_hex, private_text, 64u,
                           private_key.bytes, sizeof(private_key.bytes), &size);
  static const uint8_t identity[] = "1234567812345678";
  if (rc == H2_PAL_OK)
    rc = h2_crypto_sm2_sign(config->crypto, &private_key, identity, 16u,
                            message, message_length, &signature);
  if (rc == H2_PAL_OK) {
    packet[0] = 1u;
    rc = append_hex(data, packet, sizeof(packet) - sizeof(signature.bytes),
                    &packet_length);
    if (rc == H2_PAL_OK && message_length > sizeof(packet) -
                                                sizeof(signature.bytes) -
                                                packet_length)
      rc = H2_PAL_ERR_NO_SPACE;
  }
  if (rc == H2_PAL_OK) {
    memcpy(packet + packet_length, message, message_length);
    packet_length += message_length;
    memcpy(packet + packet_length, signature.bytes, sizeof(signature.bytes));
    packet_length += sizeof(signature.bytes);
    rc = h2_encoding_encode(&h2_encoding_base64_std, packet, packet_length,
                            encoded, sizeof(encoded), &encoded_length);
  }
  if (rc == H2_PAL_OK) {
    if (encoded_length > *length)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      memcpy(output, encoded, encoded_length);
      *length = encoded_length;
    }
  }
done:
  h2_alipay_client_wipe(message, sizeof(message));
  h2_alipay_client_wipe(packet, sizeof(packet));
  h2_alipay_client_wipe(nonce, sizeof(nonce));
  h2_alipay_client_wipe(material, sizeof(material));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  h2_alipay_client_wipe(&private_key, sizeof(private_key));
  h2_alipay_client_wipe(&signature, sizeof(signature));
  yyjson_doc_free(document);
  return rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_SPACE ? 15 : 43;
}

int h2_alipay_transit_update(const h2_alipay_client_config_t* config,
                             h2_alipay_transit_t* state, const char* number,
                             const char* type) {
  int result = transit_update_data(config, state, number, type);
  if (result != 0) return result;
  yyjson_doc* document = NULL;
  int rc = read_json(config, "transit_data", &document);
  yyjson_val* record = yyjson_obj_get(yyjson_doc_get_root(document), type);
  yyjson_val* render = member(record, "render");
  const char* mode = text(render, "renderType");
  if (rc != H2_PAL_OK || mode == NULL) {
    result = rc == H2_PAL_ERR_NO_MEMORY ? 29 : 5;
  } else if (strcmp(mode, "SCRIPT") == 0 ||
             strcmp(mode, "ONLINE_PRIORITY") == 0) {
    result = 43;
  }
  yyjson_doc_free(document);
  return result;
}

static int native_generate(const h2_alipay_client_config_t* config,
                           yyjson_val* render, uint32_t timestamp,
                           uint8_t* output, size_t* length) {
  const char* service = text(render, "certServiceType");
  if (service != NULL && strcmp(service, "YCT_OFFLINE_CERT_V1") == 0)
    return yct_generate(config, render, timestamp, output, length);
  if (service != NULL && strcmp(service, "RCT_OFFLINE_CERT_V1") == 0)
    return rct_generate(config, render, timestamp, output, length);
  const char* certificate = text(render, "offlineCert");
  const char* private_text = text(render, "certPrivateKey");
  bool version_one =
      service != NULL && strcmp(service, "ALIPAY_OFFLINE_CERT_V1") == 0;
  bool sm2 =
      service != NULL && (strcmp(service, "MOT_OFFLINE_CERT_DOUBLE_SM2") == 0 ||
                          strcmp(service, "MOT_OFFLINE_CERT_TRIPLE_SM2") == 0);
  if (service == NULL || certificate == NULL || private_text == NULL ||
      strlen(private_text) != (sm2 ? 64u : 48u) ||
      (!sm2 && !version_one && strcmp(service, "ALIPAY_OFFLINE_CERT_V2") != 0))
    return 43;
  uint8_t packet[512] = {0}, der[72] = {0};
  h2_pal_secp192k1_private_key_t private_key = {{0}};
  h2_pal_secp192k1_signature_t signature = {{0}};
  h2_pal_sm2_private_key_t sm2_private = {{0}};
  h2_pal_sm2_signature_t sm2_signature = {{0}};
  size_t key_length = 0u, size = 0u, der_length = 0u;
  int rc = h2_encoding_decode(
      &h2_encoding_hex, private_text, sm2 ? 64u : 48u,
      sm2 ? sm2_private.bytes : private_key.bytes,
      sm2 ? sizeof(sm2_private.bytes) : sizeof(private_key.bytes), &key_length);
  size_t prefix_size = version_one ? 2u : 0u;
  if (rc == H2_PAL_OK && version_one) {
    if (strlen(certificate) < 2u) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      certificate += 2u;
      packet[0] = packet[1] = 1u;
    }
  }
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, certificate, strlen(certificate),
                            packet + prefix_size,
                            sizeof(packet) - prefix_size - (sm2 ? 77u : 62u),
                            &size);
  size += prefix_size;
  if (rc == H2_PAL_OK) {
    if (!sm2) packet[size++] = 4u;
    packet[size++] = (uint8_t)(timestamp >> 24u);
    packet[size++] = (uint8_t)(timestamp >> 16u);
    packet[size++] = (uint8_t)(timestamp >> 8u);
    packet[size++] = (uint8_t)timestamp;
    static const uint8_t identity[] = "1234567812345678";
    rc = sm2 ? h2_crypto_sm2_sign(config->crypto, &sm2_private, identity, 16u,
                                  packet, size, &sm2_signature)
             : h2_pal_crypto_ecdsa_secp192k1_sha1_sign(
                   config->crypto, &private_key, packet, size, &signature);
  }
  if (rc == H2_PAL_OK && sm2) {
    der_length = sizeof(sm2_signature.bytes);
    memcpy(der, sm2_signature.bytes, der_length);
  } else if (rc == H2_PAL_OK) {
    rc = h2_crypto_ecdsa_der_encode(signature.bytes, 24u, der, sizeof(der),
                                    &der_length);
  }
  if (rc == H2_PAL_OK) {
    packet[size++] = sm2 ? 0x15u : (uint8_t)der_length;
    memcpy(packet + size, der, der_length);
    size += der_length;
    if (size > *length)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      memcpy(output, packet, size);
      *length = size;
    }
  }
  h2_alipay_client_wipe(&private_key, sizeof(private_key));
  h2_alipay_client_wipe(&signature, sizeof(signature));
  h2_alipay_client_wipe(&sm2_private, sizeof(sm2_private));
  h2_alipay_client_wipe(&sm2_signature, sizeof(sm2_signature));
  h2_alipay_client_wipe(packet, sizeof(packet));
  h2_alipay_client_wipe(der, sizeof(der));
  return rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_SPACE ? 15 : 43;
}

int h2_alipay_transit_generate(const h2_alipay_client_config_t* config,
                               h2_alipay_transit_t* state, const char* number,
                               const char* type, uint8_t* output,
                               size_t* length) {
  if (state == NULL) return 99;
  state->code_result = 99;
  if (config == NULL || number == NULL || type == NULL || output == NULL ||
      length == NULL) {
    state->code_result = 5;
    return 99;
  }
  if (strcmp(type, "S0310001") == 0) {
    state->code_result = 3;
    return 43;
  }
  yyjson_doc* document = NULL;
  int rc = read_json(config, "transit_data", &document);
  yyjson_val* record = yyjson_obj_get(yyjson_doc_get_root(document), type);
  if (rc != H2_PAL_OK || record == NULL) {
    yyjson_doc_free(document);
    document = NULL;
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
      state->code_result = 5;
      return rc == H2_PAL_ERR_NO_MEMORY ? 29 : 54;
    }
    int result = transit_update_data(config, state, number, type);
    if (result != 0) return 54;
    rc = read_json(config, "transit_data", &document);
    record = yyjson_obj_get(yyjson_doc_get_root(document), type);
  }
  yyjson_val* render = member(record, "render");
  const char* mode = text(render, "renderType");
  const char* code = text(render, "qrCode");
  bool native = mode != NULL && strcmp(mode, "NATIVE") == 0;
  bool scripted = mode != NULL && (strcmp(mode, "SCRIPT") == 0 ||
                                   strcmp(mode, "ONLINE_PRIORITY") == 0);
  int result = 43;
  if (rc != H2_PAL_OK || mode == NULL) {
    state->code_result = 5;
    result = rc == H2_PAL_ERR_NO_MEMORY ? 29 : 54;
  } else if (strcmp(mode, "ONLINE") == 0 && code != NULL) {
    size_t required = strlen(code) / 2u;
    if (*length < required) {
      state->code_result = 5;
      result = 15;
    } else {
      yyjson_doc_free(document);
      document = NULL;
      result = transit_update_data(config, state, number, type);
      if (result != 0) return 54;
      rc = read_json(config, "transit_data", &document);
      record = yyjson_obj_get(yyjson_doc_get_root(document), type);
      render = member(record, "render");
      code = text(render, "qrCode");
      size_t actual = 0u;
      rc = rc == H2_PAL_OK && code != NULL
               ? h2_encoding_decode(&h2_encoding_hex, code, strlen(code),
                                    output, *length, &actual)
               : H2_PAL_ERR_FORMAT;
      if (rc == H2_PAL_OK) {
        *length = actual;
        result = 0;
      } else {
        state->code_result = 5;
        result = rc == H2_PAL_ERR_NO_SPACE ? 15 : 54;
      }
    }
  } else {
    state->code_result = 3;
  }
  if ((native || scripted) && rc == H2_PAL_OK) {
    uint32_t timestamp = 0u;
    transit_usage_t usage = {0};
    rc = now(config, &timestamp);
    if (rc == H2_PAL_OK) rc = usage_load(config, record, &usage);
    if (rc != H2_PAL_OK) {
      state->code_result = 5;
      result = rc == H2_PAL_ERR_NO_MEMORY ? 29 : 54;
    } else {
      if (usage_expired(usage.og_checked, timestamp)) {
        usage.og_used = 0u;
        usage.og_checked = timestamp;
      }
      bool refresh =
          json_integer(member(render, "expiredTime")) < (int64_t)timestamp ||
          (usage.og_limit != 0u && usage.og_used >= usage.og_limit);
      state->code_result = 99;
      if (refresh) {
        int updated = transit_update_data(config, state, number, type);
        if (updated == 0) {
          yyjson_doc_free(document);
          document = NULL;
          rc = read_json(config, "transit_data", &document);
          record = yyjson_obj_get(yyjson_doc_get_root(document), type);
          render = member(record, "render");
          if (rc == H2_PAL_OK) rc = usage_load(config, record, &usage);
        }
      }
      if (rc == H2_PAL_OK && usage_expired(usage.mg_checked, timestamp)) {
        usage.mg_used = 0u;
        usage.mg_checked = timestamp;
      }
      if (rc != H2_PAL_OK) {
        state->code_result = 5;
        result = 54;
      } else if (usage.mg_limit != 0u && usage.mg_used >= usage.mg_limit) {
        state->code_result = 2;
        result = 40;
      } else {
        if (scripted) {
          result = 43;
        } else {
          result = native_generate(config, render, timestamp, output, length);
        }
        if (result == 0) {
          ++usage.mg_used;
          ++usage.og_used;
          const char* stored_number = text(record, "number");
          (void)store_data(config,
                           stored_number != NULL ? stored_number : number, type,
                           render, &usage);
          if (strcmp(state->last.type, type) != 0) {
            memset(&state->last, 0, sizeof(state->last));
            copy_text((uint8_t*)state->last.number, number, false);
            copy_text((uint8_t*)state->last.type, type, false);
          }
        } else {
          state->code_result = result == 15 ? 5 : 3;
        }
      }
    }
  }
  yyjson_doc_free(document);
  return result;
}

int h2_alipay_transit_last(const h2_alipay_client_config_t* config,
                           h2_alipay_transit_t* state,
                           h2_alipay_transit_card_t* output) {
  if (config == NULL || state == NULL || output == NULL) return 19;
  if (state->last.number[0] == '\0' || state->last.type[0] == '\0') return 4;
  if (state->last.title[0] == '\0') {
    void* raw = NULL;
    size_t length = 0u;
    int rc = get_blob(config, "transit_cards", &raw, &length);
    if (rc != H2_PAL_OK || length % 120u != 0u) {
      h2_pal_mem_free(config->mem, raw);
      return 5;
    }
    for (size_t index = 0u; index < length / 120u; ++index) {
      const uint8_t* card = (const uint8_t*)raw + index * 120u;
      if (memchr(card + 40u, 0, 40u) != NULL &&
          strcmp((const char*)card + 40u, state->last.type) == 0) {
        if (memchr(card + 80u, 0, 40u) != NULL)
          copy_text((uint8_t*)state->last.title, (const char*)card + 80u,
                    false);
        break;
      }
    }
    h2_pal_mem_free(config->mem, raw);
  }
  if (state->last.title[0] == '\0') return 4;
  *output = state->last;
  return 0;
}

int h2_alipay_transit_discard(const h2_alipay_client_config_t* config) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  static const char* keys[] = {"transit_count", "transit_cards", "transit_data",
                               "transit_active", "transit_scripts"};
  for (size_t index = 0u; index < sizeof(keys) / sizeof(*keys); ++index) {
    int removed = namespace->remove != NULL
                      ? namespace->remove(namespace, keys[index])
                      : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && removed != H2_PAL_ERR_NOT_FOUND) rc = removed;
  }
  if (rc == H2_PAL_OK)
    rc = namespace->commit != NULL ? namespace->commit(namespace)
                                   : H2_PAL_ERR_UNSUPPORTED;
  return h2_alipay_client_finish_namespace(namespace, rc);
}
