#include "h2_alipay_independent.h"

#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include "alipay_pay.h"
#include "alipay_pay_result.h"
#include "vendor_device_info.h"
#include <string.h>

static h2_alipay_client_config_t s_config;
static const h2_pal_sync_api_t* s_sync;
static h2_pal_mutex_t* s_lock;
static h2_alipay_client_result_t s_session;
static bool s_initialized;
static h2_alipay_independent_binding_result_t s_binding;
static h2_alipay_client_payment_t s_payment;

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
  if (rc == H2_PAL_OK) {
    if (s_session.length >= capacity) {
      rc = H2_PAL_ERR_NO_SPACE;
      h2_alipay_client_result_clear(&s_session);
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

retval_e alipay_query_order_result_string(char* pay_code,
                                          alipay_payment_status_e* status,
                                          char pay_amount[32], char* ext_data,
                                          uint32_t* ext_data_len) {
  if (pay_code == NULL || status == NULL || pay_amount == NULL ||
      ext_data_len == NULL || (ext_data == NULL && *ext_data_len != 0u))
    return ALIPAY_RV_WRONG_PARAM;
  *status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  memset(pay_amount, 0, 32u);
  if (pay_code[0] == '\0') {
    *ext_data_len = 0u;
    return 21;
  }
  int rc = h2_pal_mutex_lock(s_sync, s_lock);
  if (rc != H2_PAL_OK) return sdk_result(rc);
  memset(&s_payment, 0, sizeof(s_payment));
  if (!s_initialized)
    rc = H2_PAL_ERR_INVALID_STATE;
  else {
    size_t length = *ext_data_len;
    rc = h2_alipay_client_query_order_result(&s_config, pay_code, &s_payment,
                                             ext_data, &length);
    *ext_data_len = length <= UINT32_MAX ? (uint32_t)length : UINT32_MAX;
  }
  s_payment.result = rc;
  if (rc == H2_PAL_OK) {
    switch (s_payment.state) {
      case H2_ALIPAY_CLIENT_PAYMENT_PAID:
        *status = ALIPAY_PAYMENT_STATUS_SUCCESS;
        memcpy(pay_amount, s_payment.amount, 32u);
        break;
      case H2_ALIPAY_CLIENT_PAYMENT_PENDING:
        *status = ALIPAY_PAYMENT_STATUS_NOTHING;
        break;
      case H2_ALIPAY_CLIENT_PAYMENT_FAILED:
        *status = ALIPAY_PAYMENT_STATUS_FAIL;
        break;
      case H2_ALIPAY_CLIENT_PAYMENT_UNBOUND:
        *status = ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER;
        break;
      case H2_ALIPAY_CLIENT_PAYMENT_DISABLED:
        *status = ALIPAY_PAYMENT_STATUS_DISABLED_BY_USER;
        break;
      default:
        break;
    }
  }
  retval_e result = payment_result(rc, false);
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
  if (s_lock == NULL) return H2_PAL_OK;
  int rc = h2_pal_mutex_destroy(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_client_result_clear(&s_session);
  memset(&s_config, 0, sizeof(s_config));
  memset(&s_binding, 0, sizeof(s_binding));
  memset(&s_payment, 0, sizeof(s_payment));
  s_lock = NULL;
  s_sync = NULL;
  s_initialized = false;
  return H2_PAL_OK;
}
