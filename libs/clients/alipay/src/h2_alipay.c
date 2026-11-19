#include "h2_alipay_context.h"
#include "h2_alipay_scan.h"
#include "h2_alipay_tap.h"
#include "h2_alipay_client_internal.h"
#include "h2_alipay_tap_internal.h"
#include "h2_alipay_transit_internal.h"

#include "h2_alipay_api.h"
#include "h2_atomic_static.h"
#include "h2_encoding.h"
#include "yyjson.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static h2_alipay_client_config_t s_config;
static const h2_pal_sync_api_t* s_sync;
static h2_pal_mutex_t* s_lock;
static h2_alipay_client_result_t s_session;
static bool s_initialized;
static bool s_device_activated;
static h2_alipay_transit_t s_transit = {.card_result = 99, .code_result = 99};
static h2_alipay_independent_binding_result_t s_binding;
static h2_alipay_client_payment_t s_payment;
static h2_alipay_scan_t* s_scan;
static h2_alipay_union_t* s_union;
static h2_alipay_tap_t* s_tap;
static alipay_nfc_payment_callback_t s_nfc_callbacks;
static uint8_t s_nfc_multi_arch;
static uint32_t s_nfc_mtu = 20u;
static bool s_nfc_trusted, s_nfc_no_reminder;
static char s_nfc_debug_token[128];
static uint8_t s_sync_reply[2048];
static h2_alipay_md5_aes_keyset_t* s_nfc_keys;
static struct {
  bool first, vip;
  size_t received, expected;
  char token[64];
  uint8_t input[2048], reply[2048];
} s_nfc_ble = {.first = true};
H2_ATOMIC_DEFINE_STATIC(bool, s_query_canceled, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_transit_canceled, false);

static int nfc_setting(const char* key, bool write, uint32_t* value) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(
      s_config.pref, s_config.pref_namespace,
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
  int closed = namespace->close != NULL ? namespace->close(namespace)
                                        : H2_PAL_ERR_UNSUPPORTED;
  return rc != H2_PAL_OK ? rc : closed;
}

static int nfc_remove_guide(void) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(s_config.pref, s_config.pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  rc = namespace->remove != NULL && namespace->commit != NULL
           ? namespace->remove(namespace, "npay_guide")
           : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  int closed = namespace->close != NULL ? namespace->close(namespace)
                                        : H2_PAL_ERR_UNSUPPORTED;
  return rc != H2_PAL_OK ? rc : closed;
}

static void clear_tap_settings(void) {
  uint32_t value = 0u;
  (void)nfc_setting("npay_quick", true, &value);
  (void)nfc_setting("npay_show", true, &value);
  (void)nfc_remove_guide();
}

static void clear_scan(void) {
  h2_alipay_scan_close(s_scan);
  s_scan = NULL;
  h2_alipay_union_close(s_union);
  s_union = NULL;
  h2_alipay_tap_close(s_tap);
  s_tap = NULL;
  s_nfc_trusted = false;
  s_nfc_no_reminder = false;
  h2_alipay_client_wipe(&s_nfc_ble, sizeof(s_nfc_ble));
  s_nfc_ble.first = true;
}

static retval_e sdk_result(int result) {
  if (result == H2_PAL_OK) return ALIPAY_RV_OK;
  if (result == H2_PAL_ERR_INVALID_ARG || result == H2_PAL_ERR_NO_SPACE)
    return ALIPAY_RV_WRONG_PARAM;
  return ALIPAY_RV_HAL_ERROR;
}

static int finish(int result) {
  int unlocked = h2_pal_mutex_unlock(s_sync, s_lock);
  return result != H2_PAL_OK ? result : unlocked;
}

h2_pal_result_t h2_alipay_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync) {
  if (s_lock != NULL) return H2_PAL_ERR_INVALID_STATE;
  int valid = h2_alipay_client_validate_config(config);
  if (valid != H2_PAL_OK) return valid;
  if (sync == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mutex_config_t mutex = {
      .name = "alipay/independent",
      .allocator = config->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  h2_pal_mutex_t* lock = NULL;
  int rc = h2_pal_mutex_create(sync, &mutex, &lock);
  if (rc != H2_PAL_OK) return rc;
  if (lock == NULL) return H2_PAL_ERR_INVALID_STATE;
  s_config = *config;
  s_sync = sync;
  s_lock = lock;
  return H2_PAL_OK;
}

void alipay_binding_process_notify(int status) {
  if (h2_pal_mutex_lock(s_sync, s_lock) != H2_PAL_OK) return;
  void (*callback)(void*, int) = s_config.binding_progress;
  void* user = s_config.binding_progress_user;
  int rc = finish(H2_PAL_OK);
  if (rc == H2_PAL_OK && callback != NULL) callback(user, status);
}

static int legacy_open(bool write, h2_pal_pref_namespace_t** namespace) {
  *namespace = NULL;
  if (s_config.legacy_pref == NULL) return H2_PAL_ERR_UNSUPPORTED;
  int rc = h2_pal_pref_open(
      s_config.legacy_pref, s_config.legacy_pref_namespace,
      write ? H2_PAL_PREF_OPEN_READ_WRITE : H2_PAL_PREF_OPEN_READ_ONLY,
      namespace);
  if (rc == H2_PAL_OK && *namespace == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static int legacy_close(h2_pal_pref_namespace_t* namespace, int rc) {
  if (namespace == NULL) return rc;
  int closed = namespace->close != NULL ? namespace->close(namespace)
                                        : H2_PAL_ERR_UNSUPPORTED;
  return rc != H2_PAL_OK ? rc : closed;
}

static void legacy_key(uint32_t item, char key[13]) {
  (void)snprintf(key, 13u, "old_%08lx", (unsigned long)item);
}

static void legacy_release(void* data, size_t size) {
  if (data != NULL) {
    volatile uint8_t* bytes = data;
    for (size_t index = 0u; index < size; ++index) bytes[index] = 0u;
  }
  h2_pal_mem_free(s_config.mem, data);
}

int32_t ALIPAY_deprecated_storage_read(uint32_t item, uint8_t* out,
                                       uint32_t* length) {
  if (length == NULL || (out == NULL && *length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = legacy_open(false, &namespace);
  void* data = NULL;
  size_t size = 0u;
  char key[13];
  legacy_key(item, key);
  if (rc == H2_PAL_OK)
    rc = namespace->get_blob != NULL
             ? namespace->get_blob(namespace, s_config.mem, key, &data, &size)
             : H2_PAL_ERR_UNSUPPORTED;
  rc = legacy_close(namespace, rc);
  if (rc == H2_PAL_OK) {
    if (size > UINT32_MAX || (size != 0u && data == NULL)) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      if (size > *length)
        rc = H2_PAL_ERR_NO_SPACE;
      else if (size != 0u)
        memcpy(out, data, size);
      *length = (uint32_t)size;
    }
  }
  legacy_release(data, size);
  return finish(rc);
}

int32_t ALIPAY_deprecated_storage_write(uint32_t item, const uint8_t* data,
                                        uint32_t length) {
  if (data == NULL && length != 0u) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = legacy_open(true, &namespace);
  char key[13];
  legacy_key(item, key);
  if (rc == H2_PAL_OK) {
    rc = namespace->set_blob != NULL && namespace->commit != NULL
             ? namespace->set_blob(namespace, key, data, length)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  }
  return finish(legacy_close(namespace, rc));
}

int32_t ALIPAY_deprecated_storage_delete(uint32_t item) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = legacy_open(true, &namespace);
  char key[13];
  legacy_key(item, key);
  if (rc == H2_PAL_OK) {
    rc = namespace->remove != NULL && namespace->commit != NULL
             ? namespace->remove(namespace, key)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  }
  return finish(legacy_close(namespace, rc));
}

int32_t ALIPAY_deprecated_storage_exists(uint32_t item) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = legacy_open(false, &namespace);
  char key[13];
  legacy_key(item, key);
  void* data = NULL;
  size_t size = 0u;
  if (rc == H2_PAL_OK)
    rc = namespace->get_blob != NULL
             ? namespace->get_blob(namespace, s_config.mem, key, &data, &size)
             : H2_PAL_ERR_UNSUPPORTED;
  legacy_release(data, size);
  return finish(legacy_close(namespace, rc));
}

static yyjson_val* sync_member(yyjson_val* root, const char* name) {
  return h2_alipay_client_json_member(root, name);
}

retval_e alipay_direct_provision(const char* data) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return (retval_e)99;
  int result = h2_alipay_client_provision_receive(&s_config, data);
  return finish(H2_PAL_OK) == H2_PAL_OK ? (retval_e)result : (retval_e)99;
}

static retval_e provision_request(const char* appid, bool activate) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return (retval_e)99;
  int result = h2_alipay_client_provision_request(
      &s_config, appid, activate, &s_device_activated, &s_payment);
  return finish(H2_PAL_OK) == H2_PAL_OK ? (retval_e)result : (retval_e)99;
}

retval_e alipay_provision_ota(const char* appid) {
  return provision_request(appid, false);
}

retval_e alipay_active_device(const char* appid) {
  return provision_request(appid, true);
}

static const char* sync_text(yyjson_val* root, const char* name) {
  return yyjson_get_str(sync_member(root, name));
}

static int sync_integer(yyjson_val* value) {
  if (yyjson_is_true(value)) return 1;
  double number = yyjson_get_num(value);
  if (number >= INT_MAX) return INT_MAX;
  if (number <= INT_MIN) return INT_MIN;
  return (int)number;
}

static bool sync_add(yyjson_mut_doc* document, yyjson_mut_val* root,
                     const char* key, const char* text) {
  return yyjson_mut_obj_add_strcpy(document, root, key, text);
}

static void sync_hex(const uint8_t* input, size_t size, char* output) {
  static const char digits[] = "0123456789ABCDEF";
  for (size_t index = 0u; index < size; ++index) {
    output[index * 2u] = digits[input[index] >> 4u];
    output[index * 2u + 1u] = digits[input[index] & 15u];
  }
  output[size * 2u] = '\0';
}

static int sync_profile(yyjson_val* input, yyjson_mut_doc* document,
                        yyjson_mut_val* output) {
  h2_pal_p256_keypair_t pair = {0};
  uint8_t point[65] = {0};
  char factor[129], mac[49];
  int rc = h2_pal_crypto_p256_keypair_generate(s_config.crypto, &pair);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_p256_public_key_uncompress(s_config.crypto,
                                                  &pair.public_key, point);
  if (rc == H2_PAL_OK) {
    sync_hex(point + 1u, 64u, factor);
    if (!sync_add(document, output, "ecdhFactor", factor))
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  if (rc == H2_PAL_OK) {
    s_session.private_key = pair.private_key;
    yyjson_val* certificate = sync_member(input, "getCertificate");
    if (certificate != NULL && !yyjson_is_str(certificate))
      rc = H2_PAL_ERR_FORMAT;
    else if (certificate != NULL &&
             strcmp(yyjson_get_str(certificate), "yes") == 0)
      rc = h2_alipay_client_profile_certificate(&s_config, factor, document,
                                                output);
  }
  if (rc == H2_PAL_OK) {
    size_t size = strlen(s_config.identity.mac);
    if (size > (sizeof(mac) - 1u) / 2u)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      sync_hex((const uint8_t*)s_config.identity.mac, size, mac);
      if (!sync_add(document, output, "mac", mac)) rc = H2_PAL_ERR_NO_MEMORY;
    }
  }
  h2_alipay_client_wipe(&pair, sizeof(pair));
  h2_alipay_client_wipe(point, sizeof(point));
  h2_alipay_client_wipe(factor, sizeof(factor));
  return rc;
}

static int sync_save_profile(yyjson_val* input) {
  static const char* names[] = {"tid", "userId", "nickName", "logonId"};
  const char* encoded = sync_text(input, "encryptedFactors");
  if (encoded == NULL || sync_text(input, "serverTime") == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  binding_record_t* record = h2_pal_mem_alloc(s_config.mem, sizeof(*record));
  if (record == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(record, 0, sizeof(*record));
  char* fields[] = {record->tid, record->uid, record->nickname, record->login};
  const size_t capacities[] = {sizeof(record->tid), sizeof(record->uid),
                               sizeof(record->nickname), sizeof(record->login)};
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < 4u; ++index) {
    const char* text = sync_text(input, names[index]);
    if (text == NULL || strlen(text) >= capacities[index] ||
        (index < 2u && text[0] == '\0')) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    memcpy(fields[index], text, strlen(text) + 1u);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_alipay_client_decode_factors(&s_config, encoded, strlen(encoded),
                                         &s_session.private_key, record);
    h2_alipay_client_wipe(&s_session.private_key,
                          sizeof(s_session.private_key));
  }
  if (rc == H2_PAL_OK) {
    record->phase = BINDING_COMPLETE;
    rc = h2_alipay_client_store_binding(&s_config, record);
  }
  if (rc == H2_PAL_OK) {
    clear_scan();
    h2_alipay_client_result_clear(&s_session);
    rc = h2_alipay_client_binding_snapshot(&s_config, &s_binding.binding);
  }
  s_binding.result = rc;
  h2_alipay_client_wipe(record, sizeof(*record));
  h2_pal_mem_free(s_config.mem, record);
  return rc;
}

static bool sync_append(uint8_t* output, size_t capacity, size_t* length,
                        const char* text, size_t size) {
  if (size >= capacity - *length) return false;
  memcpy(output + *length, text, size);
  *length += size;
  return true;
}

static bool sync_indent(uint8_t* output, size_t capacity, size_t* length,
                        size_t depth) {
  for (size_t index = 0u; index < depth; ++index)
    if (!sync_append(output, capacity, length, "\t", 1u)) return false;
  return true;
}

static int sync_render(yyjson_mut_doc* document, uint8_t** output,
                       uint32_t* length, const yyjson_alc* allocator) {
  size_t size = 0u;
  char* compact = yyjson_mut_write_opts(document, 0u, allocator, &size, NULL);
  if (compact == NULL) return H2_PAL_ERR_NO_MEMORY;
  uint8_t formatted[sizeof(s_sync_reply)];
  size_t written = 0u;
  bool quoted = false, escaped = false;
  char parents[64];
  size_t depth = 0u;
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < size; ++index) {
    char byte = compact[index];
    bool emitted = false, valid = true;
    if (!quoted) {
      if (byte == '{' || byte == '[') {
        if (depth == sizeof(parents)) {
          rc = H2_PAL_ERR_NO_SPACE;
          break;
        }
        parents[depth++] = byte;
        if (byte == '{') {
          emitted = true;
          valid =
              sync_append(formatted, sizeof(formatted), &written, "{\n", 2u);
          if (index + 1u < size && compact[index + 1u] != '}')
            valid = valid &&
                    sync_indent(formatted, sizeof(formatted), &written, depth);
        }
      } else if (byte == '}' || byte == ']') {
        if (depth == 0u || parents[depth - 1u] != (byte == '}' ? '{' : '[')) {
          rc = H2_PAL_ERR_FORMAT;
          break;
        }
        --depth;
        if (byte == '}') {
          emitted = true;
          if (index != 0u && compact[index - 1u] != '{')
            valid =
                sync_append(formatted, sizeof(formatted), &written, "\n", 1u);
          valid = valid &&
                  sync_indent(formatted, sizeof(formatted), &written, depth) &&
                  sync_append(formatted, sizeof(formatted), &written, "}", 1u);
        }
      } else if (byte == ',') {
        emitted = true;
        if (depth != 0u && parents[depth - 1u] == '[')
          valid = sync_append(formatted, sizeof(formatted), &written, ", ", 2u);
        else
          valid =
              sync_append(formatted, sizeof(formatted), &written, ",\n", 2u) &&
              sync_indent(formatted, sizeof(formatted), &written, depth);
      } else if (byte == ':') {
        emitted = true;
        valid = sync_append(formatted, sizeof(formatted), &written, ":\t", 2u);
      }
    }
    if (!emitted)
      valid = sync_append(formatted, sizeof(formatted), &written, &byte, 1u);
    if (!valid) {
      rc = H2_PAL_ERR_NO_SPACE;
      break;
    }
    if (byte == '"' && !escaped) quoted = !quoted;
    if (quoted && byte == '\\' && !escaped)
      escaped = true;
    else
      escaped = false;
  }
  if (rc == H2_PAL_OK) {
    memset(s_sync_reply, 0, sizeof(s_sync_reply));
    memcpy(s_sync_reply, formatted, written);
    *output = s_sync_reply;
    *length = (uint32_t)written;
  }
  h2_alipay_client_wipe(formatted, sizeof(formatted));
  h2_alipay_client_wipe(compact, size);
  allocator->free(allocator->ctx, compact);
  return rc;
}

retval_e alipay_biz_process_entry(const uint8_t* input, uint32_t size,
                                  uint8_t** output, uint32_t* length) {
  if (size >= sizeof(s_sync_reply)) return (retval_e)BIZ_CMD_UNKNOWN;
  if (input == NULL || output == NULL || length == NULL)
    return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  if (!s_initialized) return sdk_result(finish(H2_PAL_ERR_INVALID_STATE));
  yyjson_alc allocator = {h2_alipay_client_json_alloc,
                          h2_alipay_client_json_realloc,
                          h2_alipay_client_json_free, (void*)s_config.mem};
  yyjson_doc* request = yyjson_read_opts(
      (char*)input, size, YYJSON_READ_STOP_WHEN_DONE, &allocator, NULL);
  if (request == NULL) {
    (void)finish(H2_PAL_OK);
    return (retval_e)99;
  }
  yyjson_val* root = yyjson_doc_get_root(request);
  yyjson_mut_doc* response = yyjson_mut_doc_new(&allocator);
  yyjson_mut_val* result = response != NULL ? yyjson_mut_obj(response) : NULL;
  if (result == NULL) {
    yyjson_doc_free(request);
    yyjson_mut_doc_free(response);
    (void)finish(H2_PAL_OK);
    return (retval_e)29;
  }
  yyjson_mut_doc_set_root(response, result);
  yyjson_val* command = sync_member(root, "cmd");
  int selected =
      yyjson_is_num(command) ? sync_integer(command) : BIZ_CMD_UNKNOWN;
  int operation_rc = H2_PAL_OK;
  bool valid = true;
  switch (selected) {
    case BIZ_CMD_GET_PROFILE:
      operation_rc = sync_profile(root, response, result);
      break;
    case BIZ_CMD_SAVE_PROFILE:
      operation_rc = sync_save_profile(root);
      break;
    case BIZ_CMD_LOGOUT:
      operation_rc = h2_alipay_client_discard_binding(&s_config);
      clear_tap_settings();
      clear_scan();
      h2_alipay_client_result_clear(&s_session);
      break;
    case BIZ_CMD_GET_SDK_VERSION:
      valid = sync_add(response, result, "version", "1.0.0.20229511");
      break;
    case BIZ_CMD_QUERY_BIND_STATUS: {
      const char* tid = sync_text(root, "tid");
      if (tid == NULL) {
        valid = sync_add(response, result, "bindStatus", "2");
        break;
      }
      binding_record_t binding = {0};
      operation_rc = h2_alipay_client_load_binding(&s_config, &binding);
      const char* status = operation_rc != H2_PAL_OK ||
                                   !h2_alipay_client_binding_complete(&binding)
                               ? "2"
                           : strcmp(tid, binding.tid) == 0 ? "1"
                                                           : "3";
      valid = sync_add(response, result, "bindStatus", status);
      h2_alipay_client_wipe(&binding, sizeof(binding));
      break;
    }
    case BIZ_CMD_GET_BIND_CODE: {
      char code[256];
      int count = snprintf(
          code, sizeof(code), "ALIPAY_QZWT#2#%s#QZ.SB.LY.%s-%s#4.612.8197.%u",
          s_config.identity.mac, s_config.company, s_config.identity.model,
          (unsigned)(s_config.compile_timestamp_s / 10000u));
      if (count < 0 || (size_t)count >= sizeof(code))
        operation_rc = H2_PAL_ERR_NO_SPACE;
      else
        valid = sync_add(response, result, "bindCode", code);
      break;
    }
    case BIZ_CMD_SYNC_QUICK_PAY: {
      yyjson_val* status = sync_member(root, "status");
      if (status == NULL) break;
      int setting = sync_integer(status);
      uint32_t show = (uint32_t)setting;
      operation_rc = nfc_setting("npay_show", true, &show);
      bool flag = setting == 1;
      if (flag) {
        (void)nfc_remove_guide();
        show = 2u;
        (void)nfc_setting("npay_show", true, &show);
      }
      alipay_nfc_quick_pay_flag_change_callback_t callback =
          s_nfc_callbacks.quick_pay_flag_change_callback;
      rc = finish(H2_PAL_OK);
      if (rc == H2_PAL_OK && callback != NULL) (void)callback(flag);
      if (rc == H2_PAL_OK) rc = h2_pal_mutex_lock(s_sync, s_lock);
      if (rc != H2_PAL_OK) {
        yyjson_doc_free(request);
        yyjson_mut_doc_free(response);
        return sdk_result(rc);
      }
      uint32_t quick = flag ? 1u : 0u;
      int stored = nfc_setting("npay_quick", true, &quick);
      if (operation_rc == H2_PAL_OK) operation_rc = stored;
      break;
    }
    case BIZ_CMD_GET_NFC_PAYCODE: {
      const char* token = sync_text(root, "token");
      if (token == NULL) break;
      uint8_t status = 0u, code[33] = {0};
      size_t code_length = 0u;
      (void)h2_alipay_tap_write_result(s_tap, (const uint8_t*)token,
                                       strlen(token), false, &status);
      operation_rc = h2_alipay_tap_plain_code(
          s_tap, (const uint8_t*)token, strlen(token), code, 32u, &code_length);
      if (operation_rc != H2_PAL_OK) memset(code, 0, sizeof(code));
      const char* state = status == 0u ? "0" : status == 1u ? "1" : "2";
      valid = sync_add(response, result, "status", state) &&
              sync_add(response, result, "payCode", (const char*)code) &&
              sync_add(response, result, "encrypt", "0");
      h2_alipay_client_wipe(code, sizeof(code));
      break;
    }
    default:
      break;
  }
  const char* request_id = sync_text(root, "requestId");
  valid = valid && sync_add(response, result, "result",
                            selected >= BIZ_CMD_GET_PROFILE &&
                                    selected <= BIZ_CMD_GET_NFC_PAYCODE
                                ? "0"
                                : "-1");
  if (request_id != NULL)
    valid = valid && sync_add(response, result, "requestId", request_id);
  rc = valid ? sync_render(response, output, length, &allocator)
             : H2_PAL_ERR_NO_MEMORY;
  yyjson_doc_free(request);
  yyjson_mut_doc_free(response);
  retval_e answer = rc == H2_PAL_OK              ? (retval_e)0
                    : rc == H2_PAL_ERR_NO_MEMORY ? (retval_e)29
                    : rc == H2_PAL_ERR_NO_SPACE  ? (retval_e)15
                                                 : (retval_e)99;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? answer : sdk_result(unlocked);
}

retval_e alipay_migrate_data(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  bool changed = false;
  rc = s_initialized ? h2_alipay_client_migrate_data(
                           &s_config, &s_session.private_key, &changed)
                     : H2_PAL_ERR_INVALID_STATE;
  if (changed) {
    clear_scan();
    h2_alipay_client_result_clear(&s_session);
  }
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? (retval_e)(rc == H2_PAL_OK ? 0 : 99)
                               : sdk_result(unlocked);
}

static retval_e identity_text(unsigned field, uint8_t* out, uint32_t* length) {
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  const char* values[] = {s_config.company, s_config.identity.model,
                          s_config.identity.sn, s_config.identity.imei,
                          s_config.identity.mac};
  const char* value = values[field];
  size_t size = strlen(value);
  if (size >= *length) {
    rc = H2_PAL_ERR_NO_SPACE;
  } else {
    memcpy(out, value, size + 1u);
    *length = (uint32_t)size;
  }
  return sdk_result(finish(rc));
}

retval_e alipay_get_companyName(uint8_t* out, uint32_t* length) {
  return identity_text(0u, out, length);
}
retval_e alipay_get_productModel(uint8_t* out, uint32_t* length) {
  return identity_text(1u, out, length);
}
retval_e alipay_get_sn(uint8_t* out, uint32_t* length) {
  return identity_text(2u, out, length);
}
retval_e alipay_get_imei(uint8_t* out, uint32_t* length) {
  return identity_text(3u, out, length);
}
retval_e alipay_get_mac(uint8_t* out, uint32_t* length) {
  return identity_text(4u, out, length);
}
uint32_t alipay_get_productType(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return UINT32_MAX;
  uint32_t type = s_config.product_type;
  return finish(H2_PAL_OK) == H2_PAL_OK ? type : UINT32_MAX;
}

int alipay_pre_init(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  s_initialized = true;
  return sdk_result(finish(H2_PAL_OK));
}

retval_e alipay_get_binding_code(const uint32_t feature, char* code,
                                 int* length) {
  if (code == NULL || length == NULL || *length <= 0)
    return ALIPAY_RV_WRONG_PARAM;
  size_t capacity = (size_t)*length;
  code[0] = '\0';
  *length = 0;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  if (!s_initialized) return sdk_result(finish(H2_PAL_ERR_INVALID_STATE));
  h2_alipay_client_binding_t previous;
  rc = h2_alipay_client_binding_snapshot(&s_config, &previous);
  if (rc != H2_PAL_OK) return sdk_result(finish(rc));
  if (previous.state != H2_ALIPAY_CLIENT_UNBOUND)
    return sdk_result(finish(H2_PAL_ERR_INVALID_STATE));
  h2_alipay_client_result_clear(&s_session);
  s_config.feature = feature;
  rc = h2_alipay_client_get_binding_code(&s_config, &s_session);
  s_binding.binding = previous;
  s_binding.result = rc;
  s_binding.stage = s_session.stage;
  s_binding.http_status = s_session.http_status;
  s_binding.rpc_status = s_session.rpc_status;
  s_binding.requests = s_session.requests;
  if (rc == H2_PAL_OK) {
    if (s_session.length >= capacity) {
      rc = H2_PAL_ERR_NO_SPACE;
      h2_alipay_client_result_clear(&s_session);
      s_binding.result = rc;
    } else {
      memcpy(code, s_session.code, s_session.length + 1u);
      *length = (int)s_session.length;
    }
  }
  rc = finish(rc);
  if (rc != H2_PAL_OK) {
    code[0] = '\0';
    *length = 0;
  }
  return sdk_result(rc);
}

retval_e alipay_query_binding_result(binding_status_e* status) {
  if (status == NULL) return ALIPAY_RV_WRONG_PARAM;
  *status = ALIPAY_STATUS_BINDING_FAIL;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_binding, 0, sizeof(s_binding));
  s_binding.binding.state = H2_ALIPAY_CLIENT_FAILED;
  s_binding.result = s_initialized
                         ? h2_alipay_client_query_binding_result(
                               &s_config, &s_session, &s_binding.binding)
                         : H2_PAL_ERR_INVALID_STATE;
  s_binding.stage = s_session.stage;
  s_binding.requests = s_session.requests;
  s_binding.http_status = s_session.http_status;
  s_binding.rpc_status = s_session.rpc_status;
  if (s_binding.result == H2_PAL_OK &&
      s_binding.binding.state == H2_ALIPAY_CLIENT_COMPLETE)
    *status = ALIPAY_STATUS_BINDING_OK;
  else if (s_binding.binding.state == H2_ALIPAY_CLIENT_PENDING)
    *status = ALIPAY_STATUS_UNKNOWN;
  /* The delivered wrapper returns zero and communicates business outcome
   * through status. PAL-only details remain available through the H2 API. */
  return sdk_result(finish(H2_PAL_OK));
}

h2_pal_result_t h2_alipay_independent_last_binding_result(
    h2_alipay_independent_binding_result_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  *out = s_binding;
  return finish(H2_PAL_OK);
}

h2_pal_result_t h2_alipay_independent_binding_snapshot(
    h2_alipay_client_binding_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_client_binding_t){.state =
                                          H2_ALIPAY_CLIENT_NEEDS_VERIFICATION};
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  rc = s_initialized ? h2_alipay_client_binding_snapshot(&s_config, out)
                     : H2_PAL_ERR_INVALID_STATE;
  return finish(rc);
}

bool alipay_get_binding_status(void) {
  h2_alipay_client_binding_t binding;
  return h2_alipay_independent_binding_snapshot(&binding) == H2_PAL_OK &&
         binding.state == H2_ALIPAY_CLIENT_COMPLETE;
}

static retval_e account_text(h2_alipay_client_account_field_t field,
                             uint8_t* out, uint32_t* length) {
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  size_t size = *length;
  rc = s_initialized
           ? h2_alipay_client_get_account_text(&s_config, field, out, &size)
           : H2_PAL_ERR_NOT_FOUND;
  *length = (uint32_t)size;
  retval_e result = rc == H2_PAL_OK ? ALIPAY_RV_OK : 4;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_get_logon_ID(uint8_t* out, uint32_t* length) {
  return account_text(H2_ALIPAY_CLIENT_ACCOUNT_LOGIN, out, length);
}

retval_e alipay_get_nick_name(uint8_t* out, uint32_t* length) {
  return account_text(H2_ALIPAY_CLIENT_ACCOUNT_NICKNAME, out, length);
}

static retval_e static_code(const char* text, size_t size, uint8_t* out,
                            uint32_t* length) {
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  if (*length <= size) return 15;
  memcpy(out, text, size);
  *length = (uint32_t)size;
  return ALIPAY_RV_OK;
}

retval_e alipay_get_privacy_code(uint8_t* out, uint32_t* length) {
  static const char code[] =
      "https://render.alipay.com/p/c/180021120000000183/"
      "index.html?agreementId=AG01000444";
  return static_code(code, sizeof(code) - 1u, out, length);
}

retval_e alipay_get_setting_code(uint8_t* out, uint32_t* length) {
  static const char code[] =
      "alipays://platformapi/startapp?appId=2017092008831174"
      "&page=%2Fpages%2Findex%2Findex";
  return static_code(code, sizeof(code) - 1u, out, length);
}

static retval_e reset_account(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  h2_alipay_client_result_t result;
  rc = s_initialized ? h2_alipay_client_unbind(&s_config, &result)
                     : H2_PAL_ERR_INVALID_STATE;
  h2_alipay_client_result_clear(&result);
  if (rc == H2_PAL_OK) {
    clear_scan();
    clear_tap_settings();
    h2_alipay_client_result_clear(&s_session);
    memset(&s_binding, 0, sizeof(s_binding));
    memset(&s_payment, 0, sizeof(s_payment));
    if (s_transit.registered) {
      rc = h2_alipay_transit_discard(&s_config);
      memset(&s_transit.last, 0, sizeof(s_transit.last));
    }
  }
  return sdk_result(finish(rc));
}

retval_e alipay_unbinding(void) { return reset_account(); }

retval_e alipay_reset_all(void) { return reset_account(); }

retval_e alipay_query_remote_bind_status(alipay_remote_bind_status_t* status) {
  if (status == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  h2_alipay_client_result_t result = {0};
  h2_alipay_client_remote_binding_t state = H2_ALIPAY_CLIENT_REMOTE_UNKNOWN;
  rc = s_initialized
           ? h2_alipay_client_query_remote_binding(&s_config, &state, &result)
           : H2_PAL_ERR_INVALID_STATE;
  *status = state == H2_ALIPAY_CLIENT_REMOTE_BOUND     ? REMOTE_STATUS_BIND
            : state == H2_ALIPAY_CLIENT_REMOTE_UNBOUND ? REMOTE_STATUS_UNBIND
                                                       : REMOTE_STATUS_UNKNOWN;
  if (rc == H2_PAL_OK && state == H2_ALIPAY_CLIENT_REMOTE_UNBOUND) {
    clear_scan();
    clear_tap_settings();
    h2_alipay_client_result_clear(&s_session);
    memset(&s_binding, 0, sizeof(s_binding));
    memset(&s_payment, 0, sizeof(s_payment));
  }
  h2_alipay_client_result_clear(&result);
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return sdk_result(unlocked);
  return rc == H2_PAL_OK ? ALIPAY_RV_OK : 99;
}

retval_e alipay_get_balance_bmp(char bitmap[32][14]) {
  if (bitmap == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  h2_alipay_client_result_t result = {0};
  rc = s_initialized
           ? h2_alipay_client_get_balance_bitmap(&s_config, bitmap, &result)
           : H2_PAL_ERR_INVALID_STATE;
  h2_alipay_client_result_clear(&result);
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return sdk_result(unlocked);
  return rc == H2_PAL_OK ? ALIPAY_RV_OK : 99;
}

static retval_e payment_result(int rc, bool generation) {
  if (rc == H2_PAL_OK) return ALIPAY_RV_OK;
  if (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_NO_SPACE)
    return ALIPAY_RV_WRONG_PARAM;
  if (s_payment.stage == H2_ALIPAY_CLIENT_STORAGE) return generation ? 4 : 2;
  if (s_payment.stage == H2_ALIPAY_CLIENT_QUERY_PAYMENT)
    return rc == H2_PAL_ERR_FORMAT || rc == H2_PAL_ERR_NO_MEMORY ? 29 : 7;
  /* The public vendor wrapper collapses inner paycode failures to 99. */
  if (s_payment.stage == H2_ALIPAY_CLIENT_PAYCODE_DECRYPT) return 99;
  if (s_payment.stage == H2_ALIPAY_CLIENT_PAYCODE) return 99;
  return sdk_result(rc);
}

retval_e alipay_get_paycode(uint8_t* paycode, uint32_t* len_paycode) {
  if (paycode == NULL || len_paycode == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  if (!s_initialized)
    rc = H2_PAL_ERR_INVALID_STATE;
  else {
    size_t length = *len_paycode;
    rc = h2_alipay_client_get_paycode(&s_config, paycode, &length, &s_payment);
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_SPACE)
      *len_paycode = (uint32_t)length;
  }
  s_payment.result = rc;
  retval_e result = payment_result(rc, true);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_get_collectioncode(uint8_t* code, uint32_t* length) {
  if (code == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  if (!s_initialized) {
    rc = H2_PAL_ERR_INVALID_STATE;
  } else {
    size_t size = *length;
    rc =
        h2_alipay_client_get_collectioncode(&s_config, code, &size, &s_payment);
    if (rc == H2_PAL_OK) *length = (uint32_t)size;
  }
  s_payment.result = rc;
  retval_e result = rc == H2_PAL_ERR_NO_SPACE ? 15 : payment_result(rc, true);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

static alipay_payment_status_e payment_status(void) {
  switch (s_payment.state) {
    case H2_ALIPAY_CLIENT_PAYMENT_PAID:
      return ALIPAY_PAYMENT_STATUS_SUCCESS;
    case H2_ALIPAY_CLIENT_PAYMENT_PENDING:
      return ALIPAY_PAYMENT_STATUS_NOTHING;
    case H2_ALIPAY_CLIENT_PAYMENT_FAILED:
      return ALIPAY_PAYMENT_STATUS_FAIL;
    case H2_ALIPAY_CLIENT_PAYMENT_UNBOUND:
      return ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER;
    case H2_ALIPAY_CLIENT_PAYMENT_DISABLED:
      return ALIPAY_PAYMENT_STATUS_DISABLED_BY_USER;
    case H2_ALIPAY_CLIENT_PAYMENT_PROCESSING:
      return ALIPAY_PAYMENT_STATUS_PROCESSING;
    default:
      return ALIPAY_PAYMENT_STATUS_UNKNOWN;
  }
}

static int payment_canceled(void* user) {
  (void)user;
  if (h2_atomic_bool_load(&s_query_canceled, H2_ATOMIC_ACQUIRE)) return 1;
  return s_config.cancel_cb != NULL ? s_config.cancel_cb(s_config.cancel_user)
                                    : 0;
}

static h2_alipay_client_config_t payment_config(void) {
  h2_atomic_bool_store(&s_query_canceled, false, H2_ATOMIC_RELEASE);
  h2_alipay_client_config_t config = s_config;
  config.cancel_cb = payment_canceled;
  config.cancel_user = NULL;
  return config;
}

void alipay_query_flow_break(void) {
  h2_atomic_bool_store(&s_query_canceled, true, H2_ATOMIC_RELEASE);
}

static int transit_canceled(void* user) {
  (void)user;
  if (h2_atomic_bool_load(&s_transit_canceled, H2_ATOMIC_ACQUIRE)) return 1;
  return s_config.cancel_cb != NULL ? s_config.cancel_cb(s_config.cancel_user)
                                    : 0;
}

static h2_alipay_client_config_t transit_config(void) {
  h2_atomic_bool_store(&s_transit_canceled, false, H2_ATOMIC_RELEASE);
  h2_alipay_client_config_t config = s_config;
  config.cancel_cb = transit_canceled;
  config.cancel_user = NULL;
  return config;
}

void alipay_break_rpc(void) {
  h2_atomic_bool_store(&s_transit_canceled, true, H2_ATOMIC_RELEASE);
}

int alipay_init_transit_module(void) {
  if (s_lock == NULL) {
    s_transit.registered = true;
    return 0;
  }
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  s_transit.registered = true;
  return finish(H2_PAL_OK) == H2_PAL_OK ? 0 : 99;
}

static retval_e transit_activity(bool online, bool* active) {
  if (active == NULL) return 19;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  h2_alipay_client_config_t config = transit_config();
  int result =
      s_initialized ? h2_alipay_transit_activity(&config, online, active) : 99;
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_get_activity_status_offline(bool* active) {
  return transit_activity(false, active);
}

retval_e alipay_transit_get_activity_status_online(bool* active) {
  return transit_activity(true, active);
}

static retval_e transit_cards(bool online, alipay_tansit_CardBaseVO_t* output,
                              uint32_t* length, uint32_t* count) {
  if (output == NULL || length == NULL || count == NULL) return 19;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  size_t size = *length;
  h2_alipay_client_config_t config = transit_config();
  int result = s_initialized
                   ? h2_alipay_transit_cards(&config, &s_transit, online,
                                             (uint8_t*)output, &size, count)
                   : 99;
  *length = (uint32_t)size;
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_get_card_list_offline(
    alipay_tansit_CardBaseVO_t* output, uint32_t* length, uint32_t* count) {
  return transit_cards(false, output, length, count);
}

retval_e alipay_transit_get_card_list_online(alipay_tansit_CardBaseVO_t* output,
                                             uint32_t* length,
                                             uint32_t* count) {
  return transit_cards(true, output, length, count);
}

retval_e alipay_transit_update_card_data(char* number, char* type, char* error,
                                         uint32_t error_capacity) {
  if (number == NULL || type == NULL || error == NULL || error_capacity == 0u)
    return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  h2_alipay_client_config_t config = transit_config();
  int result = s_initialized
                   ? h2_alipay_transit_update(&config, &s_transit, number, type)
                   : 99;
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_check_card_status(
    char* number, char* type, alipay_transit_card_status_t* status) {
  if (number == NULL || type == NULL || status == NULL) return 19;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  int result = s_initialized
                   ? h2_alipay_transit_status(
                         &s_config, type, &status->is_exists,
                         &status->expire_timestamp, &status->remain_use_count)
                   : 99;
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_get_TransitCode(char* number, char* type,
                                        uint8_t* output, uint32_t* length,
                                        char* error, uint32_t error_capacity) {
  if (number == NULL || type == NULL || output == NULL || length == NULL ||
      error == NULL || error_capacity == 0u)
    return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  size_t size = *length;
  h2_alipay_client_config_t config = transit_config();
  int result = s_initialized
                   ? h2_alipay_transit_generate(&config, &s_transit, number,
                                                type, output, &size)
                   : 99;
  *length = (uint32_t)size;
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_get_the_last_transcode_info(
    alipay_tansit_CardBaseVO_t* output) {
  if (output == NULL) return 19;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  h2_alipay_transit_card_t card;
  int result =
      s_initialized ? h2_alipay_transit_last(&s_config, &s_transit, &card) : 99;
  if (result == 0) {
    memcpy(output->cardNo, card.number, sizeof(card.number));
    memcpy(output->cardType, card.type, sizeof(card.type));
    memcpy(output->title, card.title, sizeof(card.title));
  }
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

retval_e alipay_transit_get_the_last_transitCode(char* title, uint8_t* output,
                                                 uint32_t* length, char* error,
                                                 uint32_t error_capacity) {
  if (title == NULL || output == NULL || length == NULL || error == NULL ||
      error_capacity == 0u)
    return 19;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  h2_alipay_transit_card_t card;
  int result =
      s_initialized ? h2_alipay_transit_last(&s_config, &s_transit, &card) : 99;
  if (result != 0) {
    s_transit.code_result = 17;
  } else {
    memcpy(title, card.title, strlen(card.title) + 1u);
    size_t size = *length;
    h2_alipay_client_config_t config = transit_config();
    result = h2_alipay_transit_generate(&config, &s_transit, card.number,
                                        card.type, output, &size);
    *length = (uint32_t)size;
  }
  return finish(H2_PAL_OK) == H2_PAL_OK ? result : 99;
}

void alipay_get_last_card_list_result(transit_card_list_result_e* output) {
  if (output == NULL) return;
  if (s_lock == NULL) {
    *output = (transit_card_list_result_e)s_transit.card_result;
    return;
  }
  if (h2_pal_mutex_lock(s_sync, s_lock) != H2_PAL_OK) return;
  *output = (transit_card_list_result_e)s_transit.card_result;
  (void)finish(H2_PAL_OK);
}

void alipay_get_last_transit_code_result(
    transit_transit_code_result_e* output) {
  if (output == NULL) return;
  if (s_lock == NULL) {
    *output = (transit_transit_code_result_e)s_transit.code_result;
    return;
  }
  if (h2_pal_mutex_lock(s_sync, s_lock) != H2_PAL_OK) return;
  *output = (transit_transit_code_result_e)s_transit.code_result;
  (void)finish(H2_PAL_OK);
}

void alipay_nfc_init_callbacks(alipay_nfc_payment_callback_t callbacks) {
  if (callbacks.send_data_handle == NULL ||
      callbacks.notify_payment_result_handle == NULL)
    return;
  if (s_lock == NULL) {
    s_nfc_callbacks = callbacks;
    return;
  }
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return;
  s_nfc_callbacks = callbacks;
  (void)finish(H2_PAL_OK);
}

retval_e alipay_nfc_setup_multi_arch(uint8_t value) {
  if (s_lock == NULL) {
    s_nfc_multi_arch = value;
    return 0;
  }
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  s_nfc_multi_arch = value;
  return sdk_result(finish(H2_PAL_OK));
}

retval_e alipay_npay_set_quick_pay_flag(bool flag) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  if (s_nfc_multi_arch == 1u) return sdk_result(finish(H2_PAL_OK));
  if (flag) {
    uint32_t show = 2u;
    (void)nfc_remove_guide();
    (void)nfc_setting("npay_show", true, &show);
  }
  alipay_nfc_quick_pay_flag_change_callback_t callback =
      s_nfc_callbacks.quick_pay_flag_change_callback;
  rc = finish(H2_PAL_OK);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  if (callback != NULL) (void)callback(flag);
  rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  uint32_t value = flag ? 1u : 0u;
  rc = nfc_setting("npay_quick", true, &value);
  return finish(rc);
}

bool alipay_npay_get_quick_pay_flag(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return false;
  uint32_t value = 0u;
  rc = nfc_setting("npay_quick", false, &value);
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return false;
  if (rc != H2_PAL_OK) {
    (void)alipay_npay_set_quick_pay_flag(false);
    return false;
  }
  return value == 1u;
}

bool alipay_npay_get_quick_pay_show_flag(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return false;
  uint32_t value = 0u;
  rc = nfc_setting("npay_show", false, &value);
  if (rc != H2_PAL_OK) {
    value = 0u;
    if (s_nfc_multi_arch != 1u) (void)nfc_setting("npay_show", true, &value);
  }
  return finish(H2_PAL_OK) == H2_PAL_OK && value != 0u;
}

static uint32_t guide_number(const char* text) {
  uint32_t value = 0u;
  bool negative = *text == '-';
  if (negative || *text == '+') ++text;
  while (*text >= '0' && *text <= '9') {
    value = value * 10u + (unsigned)(*text - '0');
    ++text;
  }
  return negative ? 0u - value : value;
}

static bool update_guide(uint32_t now, char record[128]) {
  char previous[128];
  memcpy(previous, record, sizeof(previous));
  char* parts[5];
  size_t count = 0u;
  char* cursor = previous;
  while (count < 5u) {
    parts[count++] = cursor;
    char* next = strchr(cursor, '_');
    if (next == NULL) break;
    *next = '\0';
    cursor = next + 1;
  }
  unsigned seen = previous[0] >= '1' && previous[0] <= '3'
                      ? (unsigned)(previous[0] - '0')
                      : 0u;
  bool reset = record[0] == '\0' || record[0] == '0' || count < 2u ||
               count > 4u || seen == 0u || seen >= count;
  for (size_t index = 1u; !reset && index <= seen; ++index)
    if (guide_number(parts[index]) >= now) reset = true;
  if (!reset && now - guide_number(parts[1]) > 604800u) reset = true;
  if (reset) {
    (void)snprintf(record, 128u, "1_%lu", (unsigned long)now);
    return true;
  }
  if (seen == 1u) {
    (void)snprintf(record, 128u, "2_%.32s_%lu", parts[1], (unsigned long)now);
    return true;
  }
  if (seen == 2u) {
    (void)snprintf(record, 128u, "3_%.32s_%.32s_%lu", parts[1], parts[2],
                   (unsigned long)now);
    return true;
  }
  return false;
}

bool alipay_npay_need_show_quick_open_guide(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return false;
  bool eligible = s_nfc_trusted && !s_nfc_no_reminder;
  if (finish(H2_PAL_OK) != H2_PAL_OK || !eligible ||
      alipay_npay_get_quick_pay_flag())
    return false;
  rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return false;
  uint64_t milliseconds = 0u;
  rc = h2_pal_time_get_wall_ms(s_config.time, &milliseconds);
  if (rc != H2_PAL_OK) {
    (void)finish(H2_PAL_OK);
    return false;
  }
  h2_pal_pref_namespace_t* namespace = NULL;
  rc = h2_pal_pref_open(s_config.pref, s_config.pref_namespace,
                        H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK || namespace == NULL) {
    (void)finish(H2_PAL_OK);
    return false;
  }
  char record[128] = {0};
  void* data = NULL;
  size_t size = 0u;
  if (namespace->get_blob != NULL &&
      namespace->get_blob(namespace, s_config.mem, "npay_guide", &data,
                          &size) == H2_PAL_OK &&
      size < sizeof(record))
    memcpy(record, data, size);
  h2_pal_mem_free(s_config.mem, data);
  bool show = update_guide((uint32_t)(milliseconds / 1000u), record);
  if (namespace->set_blob != NULL && namespace->commit != NULL &&
      namespace->set_blob(namespace, "npay_guide", record, strlen(record)) == 0)
    (void)namespace->commit(namespace);
  if (namespace->close != NULL) (void)namespace->close(namespace);
  return finish(H2_PAL_OK) == H2_PAL_OK && show;
}

void alipay_npay_set_ble_mtu_size(uint32_t mtu) {
  if (s_lock == NULL) {
    s_nfc_mtu = mtu > 100u ? 100u : 20u;
    return;
  }
  if (h2_pal_mutex_lock(s_sync, s_lock) != H2_PAL_OK) return;
  s_nfc_mtu = mtu > 100u ? 100u : 20u;
  (void)finish(H2_PAL_OK);
}

char* alipay_nfc_test_token_generation(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return NULL;
  rc = h2_alipay_tap_debug_token(s_config.crypto, s_nfc_debug_token);
  return finish(rc) == H2_PAL_OK ? s_nfc_debug_token : NULL;
}

retval_e alipay_query_order_result(char* pay_code,
                                   alipay_payment_status_e* status,
                                   char bitmap[32][14]) {
  if (pay_code == NULL || status == NULL || bitmap == NULL)
    return ALIPAY_RV_WRONG_PARAM;
  *status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  if (pay_code[0] == '\0') return 21;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  h2_alipay_client_config_t config = payment_config();
  rc = s_initialized ? h2_alipay_client_query_order_bitmap(&config, pay_code,
                                                           &s_payment, bitmap)
                     : H2_PAL_ERR_INVALID_STATE;
  s_payment.result = rc;
  if (rc == H2_PAL_OK) {
    *status = payment_status();
    if (s_payment.state == H2_ALIPAY_CLIENT_PAYMENT_UNBOUND) {
      clear_scan();
      clear_tap_settings();
    }
  }
  retval_e result = payment_result(rc, false);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

static retval_e query_payment_string(const char* pay_code, bool nfc,
                                     alipay_payment_status_e* status,
                                     char pay_amount[32], char* ext_data,
                                     uint32_t* ext_data_len) {
  if ((!nfc && pay_code == NULL) || status == NULL || pay_amount == NULL ||
      ext_data_len == NULL || (ext_data == NULL && *ext_data_len != 0u))
    return ALIPAY_RV_WRONG_PARAM;
  *status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  memset(pay_amount, 0, 32u);
  if (!nfc && pay_code[0] == '\0') {
    *ext_data_len = 0u;
    return 21;
  }
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  char context_code[32] = {0}, token[32] = {0};
  if (nfc) {
    if (s_tap != NULL)
      (void)h2_alipay_tap_payment_context(s_tap, context_code, token);
    pay_code = context_code;
    if (pay_code[0] == '\0') {
      *ext_data_len = 0u;
      (void)finish(H2_PAL_OK);
      return 21;
    }
  }
  memset(&s_payment, 0, sizeof(s_payment));
  if (!s_initialized)
    rc = H2_PAL_ERR_INVALID_STATE;
  else {
    h2_alipay_client_config_t config = payment_config();
    size_t length = *ext_data_len;
    rc = nfc ? h2_alipay_client_query_nfc_payment(&config, pay_code, token,
                                                  &s_payment, ext_data, &length)
             : h2_alipay_client_query_order_result(
                   &config, pay_code, &s_payment, ext_data, &length);
    if (length != 0u || s_payment.state == H2_ALIPAY_CLIENT_PAYMENT_FAILED)
      *ext_data_len = length <= UINT32_MAX ? (uint32_t)length : UINT32_MAX;
  }
  s_payment.result = rc;
  if (rc == H2_PAL_OK) {
    *status = payment_status();
    if (s_payment.state == H2_ALIPAY_CLIENT_PAYMENT_UNBOUND) {
      clear_scan();
      clear_tap_settings();
    }
    if (s_payment.state == H2_ALIPAY_CLIENT_PAYMENT_PAID)
      memcpy(pay_amount, s_payment.amount, 32u);
  }
  retval_e result = payment_result(rc, false);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_query_order_result_string(char* code,
                                          alipay_payment_status_e* status,
                                          char amount[32], char* ext,
                                          uint32_t* ext_length) {
  return query_payment_string(code, false, status, amount, ext, ext_length);
}

retval_e alipay_query_last_nfc_pay_result(alipay_payment_status_e* status,
                                          char amount[32], char* ext,
                                          uint32_t* ext_length) {
  return query_payment_string(NULL, true, status, amount, ext, ext_length);
}

retval_e alipay_query_income_details(char* code,
                                     alipay_income_detail_info_t* detail) {
  if (code == NULL || detail == NULL) return ALIPAY_RV_WRONG_PARAM;
  detail->is_success = false;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  h2_alipay_client_income_t income;
  income.success = false;
  memcpy(income.bitmap, detail->bitmap_amount, sizeof(income.bitmap));
  memcpy(income.message, detail->message, sizeof(income.message));
  rc = s_initialized
           ? h2_alipay_client_query_income(&s_config, code, &income, &s_payment)
           : H2_PAL_ERR_INVALID_STATE;
  detail->is_success = income.success;
  if (rc == H2_PAL_OK) {
    memcpy(detail->bitmap_amount, income.bitmap, sizeof(income.bitmap));
    memcpy(detail->message, income.message, sizeof(income.message));
  }
  s_payment.result = rc;
  memset(&income, 0, sizeof(income));
  retval_e result = sdk_result(rc);
  if (rc == H2_PAL_ERR_NO_MEMORY)
    result = 29;
  else if (s_payment.stage == H2_ALIPAY_CLIENT_STORAGE)
    result = 2;
  else if (s_payment.stage == H2_ALIPAY_CLIENT_QUERY_INCOME)
    result = rc == H2_PAL_OK                  ? 0
             : rc == H2_PAL_ERR_INVALID_STATE ? 26
             : rc == H2_PAL_ERR_FORMAT        ? 29
             : rc == H2_PAL_ERR_NOT_FOUND     ? 99
                                              : 7;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

static int ensure_scan(void) {
  if (!s_initialized) return H2_PAL_ERR_INVALID_STATE;
  return s_scan == NULL ? h2_alipay_scan_open(&s_config, &s_scan) : H2_PAL_OK;
}

static retval_e scan_result(int rc) {
  if (rc == H2_PAL_OK) return ALIPAY_RV_OK;
  if (rc == H2_PAL_ERR_INVALID_ARG) return ALIPAY_RV_WRONG_PARAM;
  if (rc == H2_PAL_ERR_UNSUPPORTED) return 32;
  if (s_payment.stage == H2_ALIPAY_CLIENT_STORAGE) return 2;
  if (rc == H2_PAL_ERR_NO_MEMORY || rc == H2_PAL_ERR_FORMAT) return 29;
  if (s_payment.stage == H2_ALIPAY_CLIENT_NOT_STARTED) return sdk_result(rc);
  if (rc == H2_PAL_ERR_NOT_FOUND) return 99;
  if (rc == H2_PAL_ERR_INVALID_STATE) return 26;
  return 7;
}

retval_e alipay_dzs_get_precheck_status(uint8_t* enabled) {
  if (enabled == NULL) return ALIPAY_RV_WRONG_PARAM;
  *enabled = 0u;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  rc = ensure_scan();
  if (rc == H2_PAL_OK)
    rc = h2_alipay_scan_dzs_precheck(s_scan, enabled, &s_payment);
  s_payment.result = rc;
  retval_e result = scan_result(rc);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_dzs_generate_payment_qrcode(const char* amount,
                                            alipay_dzs_code_info_t* code) {
  if (amount == NULL || code == NULL) return ALIPAY_RV_WRONG_PARAM;
  code->is_success = false;
  code->err_code = ALIPAY_FAIL_RES_COMMON_FAIL;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  h2_alipay_scan_code_t generated = {.failure = H2_ALIPAY_SCAN_COMMON_FAILURE};
  rc = ensure_scan();
  if (rc == H2_PAL_OK)
    rc = h2_alipay_scan_dzs_generate(s_scan, amount, &generated, &s_payment);
  code->is_success = generated.success;
  code->err_code = (alipay_scan_pay_fail_res_e)generated.failure;
  if (rc == H2_PAL_OK && generated.length != 0u)
    memcpy(code->qr_code, generated.code, generated.length);
  memset(&generated, 0, sizeof(generated));
  s_payment.result = rc;
  retval_e result = scan_result(rc);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_dzs_query_latest_payment_result(
    alipay_payment_status_e* status) {
  if (status == NULL) return ALIPAY_RV_WRONG_PARAM;
  *status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  rc = ensure_scan();
  if (rc == H2_PAL_OK) rc = h2_alipay_scan_dzs_query(s_scan, &s_payment);
  *status = payment_status();
  s_payment.result = rc;
  retval_e result = scan_result(rc);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

void alipay_dzs_cancel_latest_payment(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return;
  memset(&s_payment, 0, sizeof(s_payment));
  rc = ensure_scan();
  if (rc == H2_PAL_OK) rc = h2_alipay_scan_dzs_cancel(s_scan, &s_payment);
  s_payment.result = rc;
  (void)finish(H2_PAL_OK);
}

static retval_e union_parse_locked(const char* input, bool nfc,
                                   alipay_union_code_order_info_t* order) {
  memset(&s_payment, 0, sizeof(s_payment));
  int rc = s_initialized ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && s_union == NULL)
    rc = h2_alipay_union_open(&s_config, &s_union);
  h2_alipay_scan_order_t parsed;
  _Static_assert(sizeof(parsed) == sizeof(*order), "Scan order ABI");
  memcpy(&parsed, order, sizeof(parsed));
  if (rc == H2_PAL_OK)
    rc = h2_alipay_union_parse(s_union, input, nfc, &parsed, &s_payment);
  memcpy(order, &parsed, sizeof(parsed));
  memset(&parsed, 0, sizeof(parsed));
  s_payment.result = rc;
  retval_e result = s_payment.stage == H2_ALIPAY_CLIENT_SCAN_ROUTE &&
                            (rc == H2_PAL_ERR_IO || rc == H2_PAL_ERR_FORMAT)
                        ? 99
                        : scan_result(rc);
  return result;
}

static retval_e union_parse(char* input, bool nfc,
                            alipay_union_code_order_info_t* order) {
  if (input == NULL || order == NULL || input[0] == '\0') return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  retval_e result = union_parse_locked(input, nfc, order);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_union_code_parse_with_img(
    uint8_t* image, uint32_t width, uint32_t height,
    alipay_union_code_order_info_t* order) {
  if (image == NULL || order == NULL || width == 0u || height == 0u ||
      width > INT_MAX || height > INT_MAX ||
      (size_t)width > SIZE_MAX / (size_t)height)
    return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  s_payment.result =
      s_initialized ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_INVALID_STATE;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? 99 : sdk_result(unlocked);
}

retval_e alipay_union_code_parse_with_code(
    char* code, alipay_union_code_order_info_t* order) {
  return union_parse(code, false, order);
}

retval_e alipay_nfc_parse_with_content(char* content,
                                       alipay_union_code_order_info_t* order) {
  return union_parse(content, true, order);
}

retval_e alipay_nfc_url_parse(char* url,
                              alipay_union_code_order_info_t* order) {
  return union_parse(url, true, order);
}

retval_e alipay_union_code_confirm_payment(
    const alipay_union_code_order_info_t* order,
    alipay_union_code_pay_result_t* payment) {
  if (order == NULL || payment == NULL) return 99;
  payment->status = ALIPAY_SCAN_PAYMENT_STATUS_FAIL;
  payment->res_fail = ALIPAY_FAIL_RES_COMMON_FAIL;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  h2_alipay_scan_order_t parsed;
  memcpy(&parsed, order, sizeof(parsed));
  h2_alipay_scan_failure_t failure = H2_ALIPAY_SCAN_COMMON_FAILURE;
  memset(&s_payment, 0, sizeof(s_payment));
  rc = s_initialized && s_union != NULL
           ? h2_alipay_union_confirm(s_union, &parsed, &failure, &s_payment)
           : H2_PAL_ERR_NOT_FOUND;
  payment->status = (alipay_scan_payment_status_e)payment_status();
  payment->res_fail = failure == H2_ALIPAY_SCAN_ACCOUNT_MISSING
                          ? ALIPAY_FAIL_RES_COMMON_FAIL
                          : (alipay_scan_pay_fail_res_e)failure;
  if (failure == H2_ALIPAY_SCAN_ACCOUNT_MISSING) {
    clear_scan();
    clear_tap_settings();
  }
  memset(&parsed, 0, sizeof(parsed));
  s_payment.result = rc;
  retval_e result = scan_result(rc);
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

retval_e alipay_nfc_confirm_payment(const alipay_union_code_order_info_t* order,
                                    alipay_union_code_pay_result_t* payment) {
  return alipay_union_code_confirm_payment(order, payment);
}

static int ensure_tap(void) {
  if (!s_initialized) return H2_PAL_ERR_INVALID_STATE;
  return s_tap != NULL ? H2_PAL_OK : h2_alipay_tap_open(&s_config, &s_tap);
}

static int ensure_nfc_keys(void) {
  if (s_nfc_keys != NULL) return H2_PAL_OK;
  static const h2_alipay_key_material_t profile[] = {
      {(const uint8_t*)"IMF8dz2OZyvF2wQp", 16u},
      {(const uint8_t*)"PmAQLIRXQSma971r", 16u},
      {(const uint8_t*)"ShsKXm2LJVrmQGw7", 16u}};
  return h2_alipay_md5_aes_keyset_open(s_config.mem, s_config.crypto, profile,
                                       3u, &s_nfc_keys);
}

retval_e alipay_npay_ble_broadcast_init(void) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return (retval_e)99;
  rc = ensure_nfc_keys();
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return (retval_e)99;
  return rc == H2_PAL_OK              ? (retval_e)0
         : rc == H2_PAL_ERR_NO_MEMORY ? (retval_e)30
                                      : (retval_e)99;
}

retval_e alipay_get_paycode_with_nfc_token(uint8_t* token,
                                           uint32_t token_length,
                                           uint8_t output[32],
                                           uint32_t* output_length) {
  if (token == NULL || token_length == 0u || token[0] == 0u || output == NULL ||
      output_length == NULL)
    return (retval_e)99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return (retval_e)99;
  char current_token[32] = {0}, code[32] = {0};
  rc = h2_alipay_tap_payment_context(s_tap, code, current_token);
  if (rc == H2_PAL_OK && (current_token[0] == '\0' || code[0] == '\0' ||
                          strlen(current_token) != token_length ||
                          memcmp(current_token, token, token_length) != 0))
    rc = H2_PAL_ERR_NOT_FOUND;
  if (rc == H2_PAL_OK) rc = ensure_nfc_keys();
  uint8_t cipher[48] = {0};
  char encoded[64] = {0};
  size_t cipher_length = 0u, length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_md5_aes_keyset_encrypt(s_nfc_keys, 0u, (const uint8_t*)code,
                                          strlen(code), cipher, sizeof(cipher),
                                          &cipher_length);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_base64_std, cipher, cipher_length,
                            encoded, sizeof(encoded), &length);
  if (rc == H2_PAL_OK) {
    memcpy(output, encoded, length);
    *output_length = (uint32_t)length;
  }
  h2_alipay_client_wipe(code, sizeof(code));
  h2_alipay_client_wipe(cipher, sizeof(cipher));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  int unlocked = finish(H2_PAL_OK);
  return rc == H2_PAL_OK && unlocked == H2_PAL_OK ? (retval_e)0 : (retval_e)99;
}

static void nfc_status(const alipay_nfc_payment_callback_t* callbacks,
                       alipay_nfc_pay_status_t state,
                       alipay_nfc_pay_error_t error) {
  if (callbacks->notify_payment_result_handle == NULL) return;
  alipay_payment_status_t status = {.status = state, .code = error};
  (void)callbacks->notify_payment_result_handle(&status);
}

static void nfc_observe_tag(const h2_alipay_tap_tag_t* tag) {
  if (!tag->flags_present) return;
  s_nfc_trusted = tag->trusted;
  if (!tag->trusted) return;
  uint32_t show = 0u;
  (void)nfc_setting("npay_show", false, &show);
  s_nfc_no_reminder = show > 1u;
  if (show == 0u && s_nfc_multi_arch != 1u) {
    show = 1u;
    (void)nfc_setting("npay_show", true, &show);
  }
}

static int nfc_receive(alipay_nfc_pay_reader_context_t context,
                       const uint8_t* data, uint32_t length, uint8_t* amount) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  alipay_nfc_payment_callback_t callbacks = s_nfc_callbacks;
  h2_alipay_client_binding_t binding = {0};
  rc = s_initialized ? h2_alipay_client_binding_snapshot(&s_config, &binding)
                     : H2_PAL_ERR_INVALID_STATE;
  if (rc != H2_PAL_OK || binding.state != H2_ALIPAY_CLIENT_COMPLETE) {
    (void)finish(rc);
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL,
               NFC_PAYMENT_FAIL_ERROR_NOT_BIND);
    return 99;
  }
  s_nfc_trusted = false;
  h2_alipay_tap_tag_details_t details;
  rc = h2_alipay_tap_decode_details(&s_config, data, length, &details);
  if (rc == H2_PAL_OK) nfc_observe_tag(&details.tag);
  if (context == NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET && amount != NULL)
    memset(amount, 0, 16u);
  alipay_nfc_pay_error_t error = NFC_PAYMENT_FAIL_ERROR_UNKNOWN_TAG;
  if (rc == H2_PAL_OK && details.tag.type == 5u) {
    if (context != NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET) {
      s_nfc_ble.first = true;
      s_nfc_ble.received = s_nfc_ble.expected = 0u;
    }
    error = NFC_PAYMENT_FAIL_ERROR_UNSUPPORT_WITHOUT_NETWORK;
    rc = H2_PAL_ERR_UNSUPPORTED;
  } else if (rc == H2_PAL_OK && details.tag.type == 4u) {
    bool wallet = context == NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET;
    if (context == NFC_PAY_READER_CONTEXT_LOCK_SCREEN) {
      (void)finish(H2_PAL_OK);
      nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL,
                 NFC_PAYMENT_FAIL_ERROR_HUOGUI_TAG);
      return 99;
    }
    char reply[128] = {0};
    size_t reply_length = 0u;
    rc = ensure_tap();
    if (rc == H2_PAL_OK)
      rc = h2_alipay_tap_generate(s_tap, details.tag.token,
                                  s_nfc_multi_arch != 0u, &s_payment);
    if (rc == H2_PAL_OK)
      rc = h2_alipay_tap_cabinet_reply(s_tap, wallet, reply, &reply_length);
    int unlocked = finish(H2_PAL_OK);
    if (unlocked != H2_PAL_OK) return 99;
    if (rc == H2_PAL_OK && callbacks.send_data_handle != NULL) {
      (void)callbacks.send_data_handle((const uint8_t*)reply,
                                       (uint32_t)reply_length);
      nfc_status(&callbacks, NFC_PAYMENT_STATUS_SUCCESS,
                 NFC_PAYMENT_FAIL_ERROR_HUOGUI_TAG);
    } else if (rc != H2_PAL_OK) {
      nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL,
                 NFC_PAYMENT_FAIL_ERROR_NOT_BIND);
    }
    h2_alipay_client_wipe(reply, sizeof(reply));
    return wallet || rc == H2_PAL_OK ? 0 : 99;
  } else if (rc == H2_PAL_OK &&
             ((details.tag.type != 2u && details.tag.type != 3u) ||
              details.tag.token[0] == '\0')) {
    rc = H2_PAL_ERR_FORMAT;
  }
  if (rc != H2_PAL_OK) {
    (void)finish(rc);
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL, error);
    return 99;
  }
  if (context != NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET && amount != NULL)
    memcpy(amount, details.amount, 16u);
  s_nfc_ble.first = true;
  s_nfc_ble.received = s_nfc_ble.expected = 0u;
  s_nfc_ble.vip = details.tag.type == 3u;
  if (context != NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET)
    memcpy(s_nfc_ble.token, details.tag.token, sizeof(s_nfc_ble.token));
  bool trusted = s_nfc_trusted;
  if (finish(H2_PAL_OK) != H2_PAL_OK) return 99;
  bool quick = trusted && alipay_npay_get_quick_pay_flag();
  rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  uint8_t watch[2] = {
      1u,
      (uint8_t)(0x14u |
                (context == NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET ? 1u
                                                                        : 0u) |
                (quick ? 0u : 2u))};
  bool send_nfc = context == NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET ||
                  (context != NFC_PAY_READER_CONTEXT_LOCK_SCREEN && quick);
  rc = ensure_tap();
  if (rc == H2_PAL_OK)
    rc = h2_alipay_tap_generate(s_tap, details.tag.token,
                                s_nfc_multi_arch != 0u, &s_payment);
  if (rc != H2_PAL_OK) {
    (void)finish(rc);
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL,
               send_nfc ? NFC_PAYMENT_FAIL_ERROR_OTHER
                        : NFC_PAYMENT_FAIL_ERROR_NOT_BIND);
    return 99;
  }
  char nfc_reply[128] = {0};
  size_t nfc_length = 0u;
  int nfc_result = send_nfc
                       ? h2_alipay_tap_nfc_reply(s_tap, details.tag.type == 3u,
                                                 watch, nfc_reply, &nfc_length)
                       : H2_PAL_OK;
  uint8_t ble_reply[20] = {0};
  int ble_result = ensure_nfc_keys();
  if (ble_result == H2_PAL_OK)
    ble_result =
        h2_alipay_tap_ble_reply(s_tap, s_nfc_keys, details.tag.type == 3u,
                                details.reply_mode, watch, ble_reply);
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return 99;
  if (send_nfc && nfc_result == H2_PAL_OK &&
      callbacks.send_data_handle != NULL) {
    (void)callbacks.send_data_handle((const uint8_t*)nfc_reply,
                                     (uint32_t)nfc_length);
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_SUCCESS,
               NFC_PAYMENT_FAIL_ERROR_SUCCESS);
  }
  if (ble_result == H2_PAL_OK &&
      callbacks.ble_broadcast_direct_pay_data_handle != NULL) {
    const char* uuid = details.tag.type == 3u
                           ? "00000e60-0000-1000-8000-00805f9b34fb"
                           : "0000fe3c-0000-1000-8000-00805f9b34fb";
    (void)callbacks.ble_broadcast_direct_pay_data_handle(
        23, ble_reply, sizeof(ble_reply), (const uint8_t*)uuid, 36u,
        quick ? 1u : 0u);
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_SUCCESS,
               NFC_PAYMENT_FAIL_ERROR_SUCCESS);
  }
  h2_alipay_client_wipe(nfc_reply, sizeof(nfc_reply));
  h2_alipay_client_wipe(ble_reply, sizeof(ble_reply));
  if (send_nfc) return nfc_result == H2_PAL_OK ? 0 : 99;
  return ble_result == H2_PAL_OK ? 0 : 99;
}

int alipay_nfc_recv_data_handle(const uint8_t* data, uint32_t length) {
  return nfc_receive(NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET, data, length,
                     NULL);
}

int alipay_nfc_recv_data_handle_with_context(
    alipay_nfc_pay_reader_context_t context, const uint8_t* data,
    uint32_t length, uint8_t amount[16]) {
  return nfc_receive(context, data, length, amount);
}

int alipay_npay_ble_recv_data_handle(const uint8_t* data, uint32_t length) {
  if (data == NULL || length == 0u) return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return 99;
  if (s_nfc_ble.first) {
    if (length < 10u || length > sizeof(s_nfc_ble.input)) {
      (void)finish(H2_PAL_ERR_INVALID_ARG);
      return 99;
    }
    uint32_t payload = ((uint32_t)data[6] << 24u) | ((uint32_t)data[7] << 16u) |
                       ((uint32_t)data[8] << 8u) | data[9];
    if (payload > sizeof(s_nfc_ble.input) - 11u) {
      (void)finish(H2_PAL_ERR_NO_SPACE);
      return 99;
    }
    s_nfc_ble.first = false;
    s_nfc_ble.expected = (size_t)payload + 10u;
    s_nfc_ble.received = length;
    h2_alipay_client_wipe(s_nfc_ble.input, sizeof(s_nfc_ble.input));
    memcpy(s_nfc_ble.input, data, length);
    if (s_nfc_ble.received != s_nfc_ble.expected)
      return finish(H2_PAL_OK) == H2_PAL_OK ? 0 : 99;
  } else if (s_nfc_ble.received < s_nfc_ble.expected) {
    if (length >= sizeof(s_nfc_ble.input) - s_nfc_ble.received) {
      (void)finish(H2_PAL_ERR_NO_SPACE);
      return 99;
    }
    memcpy(s_nfc_ble.input + s_nfc_ble.received, data, length);
    s_nfc_ble.received += length;
    if (s_nfc_ble.received < s_nfc_ble.expected)
      return finish(H2_PAL_OK) == H2_PAL_OK ? 0 : 99;
  }
  size_t payload_length = s_nfc_ble.received - 10u;
  char* payload = h2_pal_mem_alloc(s_config.mem, payload_length + 1u);
  if (payload == NULL) {
    (void)finish(H2_PAL_ERR_NO_MEMORY);
    return 30;
  }
  memcpy(payload, s_nfc_ble.input + 10u, payload_length);
  payload[payload_length] = '\0';
  char* parts[5] = {payload};
  size_t count = 1u;
  for (char* cursor = payload; *cursor != '\0'; ++cursor) {
    if (*cursor != ',') continue;
    *cursor = '\0';
    if (count < 5u) parts[count++] = cursor + 1u;
  }
  size_t prefix_length = count == 5u ? (size_t)(parts[3] - payload) : 0u;
  rc = count == 5u
           ? h2_alipay_tap_verify_device(s_config.crypto, parts[3], parts[4])
           : H2_PAL_ERR_FORMAT;
  int result = 52;
  size_t encrypted_length = 0u, reply_length = 0u;
  if (rc == H2_PAL_OK) {
    memset(&s_payment, 0, sizeof(s_payment));
    rc = ensure_tap();
    if (rc == H2_PAL_OK && prefix_length <= sizeof(s_nfc_ble.reply) - 266u)
      rc = h2_alipay_tap_connected_reply(
          s_tap, parts[3], s_nfc_ble.token, s_nfc_ble.vip,
          s_nfc_ble.reply + 10u + prefix_length, &encrypted_length, &s_payment);
    else if (rc == H2_PAL_OK)
      rc = H2_PAL_ERR_NO_SPACE;
    result = rc == H2_PAL_OK ? 0 : 99;
    if (rc == H2_PAL_OK) {
      size_t size = prefix_length + encrypted_length;
      memcpy(s_nfc_ble.reply, s_nfc_ble.input, prefix_length + 10u);
      for (size_t index = 0u; index < 4u; ++index)
        s_nfc_ble.reply[6u + index] = (uint8_t)(size >> (24u - index * 8u));
      reply_length = size + 10u;
    }
  }
  alipay_nfc_payment_callback_t callbacks = s_nfc_callbacks;
  uint32_t mtu = s_nfc_mtu;
  h2_alipay_client_wipe(payload, payload_length + 1u);
  h2_pal_mem_free(s_config.mem, payload);
  int unlocked = finish(H2_PAL_OK);
  if (unlocked != H2_PAL_OK) return 99;
  if (result == 0 && callbacks.ble_notify_data_handle != NULL) {
    uint8_t packet[100];
    size_t fragments = (reply_length + mtu - 3u) / (mtu - 2u);
    size_t offset = 0u;
    for (size_t index = 0u; offset < reply_length; ++index) {
      size_t remaining = reply_length - offset;
      size_t size = remaining < mtu - 2u ? remaining : mtu - 2u;
      memcpy(packet, s_nfc_ble.reply + offset, size);
      packet[size] = (uint8_t)index;
      packet[size + 1u] = (uint8_t)fragments;
      (void)callbacks.ble_notify_data_handle(index == 0u, remaining <= mtu,
                                             packet, (uint32_t)size + 2u);
      offset += size;
    }
    h2_alipay_client_wipe(packet, sizeof(packet));
  } else if (result == 99 && s_payment.result != H2_PAL_OK) {
    nfc_status(&callbacks, NFC_PAYMENT_STATUS_FAIL,
               NFC_PAYMENT_FAIL_ERROR_OTHER);
  }
  h2_alipay_client_wipe(s_nfc_ble.reply, sizeof(s_nfc_ble.reply));
  return result;
}

h2_pal_result_t h2_alipay_independent_set_nfc_context(const char* token,
                                                      const char* squeeze,
                                                      const char* paycode) {
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  rc = ensure_tap();
  h2_alipay_client_binding_t binding;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_client_binding_snapshot(&s_config, &binding);
  if (rc == H2_PAL_OK && binding.state != H2_ALIPAY_CLIENT_COMPLETE)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_tap_set_context(s_tap, token, squeeze, paycode);
  return finish(rc);
}

retval_e alipay_npay_ble_scan_pay_res_data_handle_v2(
    const uint8_t* data, uint32_t length,
    alipay_npay_ble_pay_result_t* result) {
  if (result == NULL) return ALIPAY_RV_WRONG_PARAM;
  memset(result, 0, sizeof(*result));
  if (data == NULL || length != 27u || data[0] != 1u || data[3] > 1u)
    return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  rc = ensure_tap();
  h2_alipay_tap_payment_t payment = {0};
  if (rc == H2_PAL_OK)
    rc = h2_alipay_tap_parse_result(s_tap, data, length, &payment);
  if (rc == H2_PAL_OK) {
    result->pay_result_type = payment.type;
    result->coupon_status = payment.coupon_status;
    result->order_amount_fen = payment.order_amount_fen;
    result->real_pay_amount_fen = payment.real_amount_fen;
  }
  retval_e outcome = rc == H2_PAL_OK                 ? 0
                     : rc == H2_ALIPAY_TAP_ERR_TOKEN ? 52
                     : rc == H2_PAL_ERR_UNSUPPORTED  ? 57
                                                     : 2;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? outcome : sdk_result(unlocked);
}

retval_e alipay_npay_ble_scan_pay_res_data_handle(const uint8_t* data,
                                                  uint32_t length,
                                                  uint32_t* amount) {
  if (data == NULL || length < 27u || amount == NULL)
    return ALIPAY_RV_WRONG_PARAM;
  *amount = 0u;
  alipay_npay_ble_pay_result_t payment;
  retval_e rc =
      alipay_npay_ble_scan_pay_res_data_handle_v2(data, 27u, &payment);
  if (rc == 0) *amount = payment.order_amount_fen;
  return rc;
}

retval_e alipay_npay_get_coupon_detail_url(uint8_t out[256], uint32_t* length) {
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  rc = ensure_tap();
  size_t size = *length;
  if (rc == H2_PAL_OK) rc = h2_alipay_tap_coupon_url(s_tap, out, size, &size);
  if (rc == H2_PAL_OK) *length = (uint32_t)size;
  retval_e outcome = rc == H2_PAL_OK ? 0 : rc == H2_PAL_ERR_NO_SPACE ? 15 : 2;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? outcome : sdk_result(unlocked);
}

retval_e alipay_get_nfc_available_nearby_locations_url(uint8_t out[256],
                                                       uint32_t* length) {
  static const char url[] =
      "alipays://platformapi/startapp?appId=2021004148648165&"
      "page=pages%2Findex%2Findex%3FchInfo%3Dwatch%26showDialog%3Dfalse";
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  if (*length < 256u) return 15;
  memcpy(out, url, sizeof(url));
  *length = sizeof(url) - 1u;
  return 0;
}

retval_e alipay_get_plain_paycode_with_nfc_token(uint8_t* token,
                                                 uint32_t token_length,
                                                 uint8_t out[32],
                                                 uint32_t* length) {
  if (token == NULL || token_length == 0u || token_length >= 32u ||
      out == NULL || length == NULL)
    return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  rc = ensure_tap();
  size_t size = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_tap_plain_code(s_tap, token, token_length, out, 32u, &size);
  if (rc == H2_PAL_OK) *length = (uint32_t)size;
  retval_e outcome = rc == H2_PAL_OK ? 0 : 99;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? outcome : sdk_result(unlocked);
}

retval_e alipay_get_nfc_write_result_with_nfc_token(uint8_t* token,
                                                    uint32_t token_length,
                                                    uint8_t* value) {
  if (token == NULL || token_length == 0u || token_length >= 32u ||
      value == NULL)
    return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  rc = s_tap != NULL ? h2_alipay_tap_write_result(s_tap, token, token_length,
                                                  false, value)
                     : H2_PAL_ERR_NOT_FOUND;
  retval_e outcome = rc == H2_PAL_OK ? 0 : 99;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? outcome : sdk_result(unlocked);
}

retval_e alipay_set_nfc_write_result_with_nfc_data(uint8_t* data,
                                                   uint32_t length,
                                                   uint8_t value) {
  if (data == NULL || length == 0u) return 99;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  h2_alipay_tap_tag_t tag;
  rc = h2_alipay_tap_decode_tag(&s_config, data, length, &tag);
  if (rc == H2_PAL_OK) nfc_observe_tag(&tag);
  if (rc == H2_PAL_OK)
    rc = s_tap != NULL
             ? h2_alipay_tap_write_result(s_tap, (const uint8_t*)tag.token,
                                          strlen(tag.token), true, &value)
             : H2_PAL_ERR_NOT_FOUND;
  retval_e outcome = rc == H2_PAL_OK ? 0 : 99;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? outcome : sdk_result(unlocked);
}

retval_e alipay_get_aid_code(char out[256], uint32_t* length, bool ble) {
  if (out == NULL || length == NULL) return ALIPAY_RV_WRONG_PARAM;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  size_t size = *length;
  rc = s_initialized ? h2_alipay_client_get_aid_code(&s_config, ble, out, &size)
                     : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_SPACE) *length = (uint32_t)size;
  retval_e result = rc == H2_PAL_OK              ? 0
                    : rc == H2_PAL_ERR_NO_SPACE  ? 15
                    : rc == H2_PAL_ERR_NO_MEMORY ? 29
                                                 : 99;
  int unlocked = finish(H2_PAL_OK);
  return unlocked == H2_PAL_OK ? result : sdk_result(unlocked);
}

h2_pal_result_t h2_alipay_independent_last_payment_result(
    h2_alipay_client_payment_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  *out = s_payment;
  return finish(H2_PAL_OK);
}

h2_pal_result_t h2_alipay_independent_release(void) {
  if (s_lock == NULL) {
    memset(&s_nfc_callbacks, 0, sizeof(s_nfc_callbacks));
    s_nfc_multi_arch = 0u;
    s_nfc_mtu = 20u;
    memset(s_nfc_debug_token, 0, sizeof(s_nfc_debug_token));
    memset(s_sync_reply, 0, sizeof(s_sync_reply));
    s_transit = (h2_alipay_transit_t){.card_result = 99, .code_result = 99};
    return H2_PAL_OK;
  }
  int rc = h2_pal_mutex_destroy(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  clear_scan();
  h2_alipay_md5_aes_keyset_close(s_nfc_keys);
  s_nfc_keys = NULL;
  h2_alipay_client_result_clear(&s_session);
  memset(&s_config, 0, sizeof(s_config));
  memset(&s_binding, 0, sizeof(s_binding));
  memset(&s_payment, 0, sizeof(s_payment));
  memset(&s_nfc_callbacks, 0, sizeof(s_nfc_callbacks));
  s_nfc_multi_arch = 0u;
  s_nfc_mtu = 20u;
  memset(s_nfc_debug_token, 0, sizeof(s_nfc_debug_token));
  h2_alipay_client_wipe(s_sync_reply, sizeof(s_sync_reply));
  s_lock = NULL;
  s_sync = NULL;
  s_initialized = false;
  s_device_activated = false;
  s_transit = (h2_alipay_transit_t){.card_result = 99, .code_result = 99};
  return H2_PAL_OK;
}
