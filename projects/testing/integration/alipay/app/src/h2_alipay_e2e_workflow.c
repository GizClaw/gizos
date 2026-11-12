#include "h2_alipay_e2e.h"
#include <string.h>

static uint64_t now(h2_runtime_t* runtime) {
  uint64_t ms = 0;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &ms);
  return ms;
}

static int request_code(h2_runtime_t* runtime,
                        const h2_alipay_e2e_config_t* config,
                        h2_alipay_e2e_result_t* result) {
  const h2_alipay_api_t* api = config->alipay;
  result->stage = H2_ALIPAY_E2E_GET_CODE;
  result->code_presented = result->qr_visible = false;
  uint64_t started = now(runtime);
  result->result = api->vtable->get_binding_code(
      api->user, config->feature, result->code, sizeof(result->code),
      &result->length, &result->code_sdk_rc);
  result->code_ms = now(runtime) - started;
  if (result->result == H2_PAL_OK && result->code_sdk_rc != 0)
    result->result = H2_PAL_ERR_IO;
  if (result->result == H2_PAL_OK &&
      (result->length == 0 || result->length >= sizeof(result->code) ||
       memchr(result->code, '\0', result->length) != NULL))
    result->result = H2_PAL_ERR_FORMAT;
  if (result->result != H2_PAL_OK) {
    memset(result->code, 0, sizeof(result->code));
    result->length = 0;
    return result->result;
  }
  result->code[result->length] = '\0';
  result->stage = H2_ALIPAY_E2E_CODE_READY;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_e2e_execute(h2_runtime_t* runtime,
                                      const h2_alipay_e2e_config_t* config,
                                      h2_alipay_e2e_result_t* result) {
  if (result == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  result->prepare_rc = result->init_rc = result->init_sdk_rc =
      result->reentry_rc = result->reentry_sdk_rc = result->code_sdk_rc =
          H2_ALIPAY_E2E_NOT_CALLED;
  result->display_rc = H2_ALIPAY_E2E_NOT_CALLED;
  result->result = H2_PAL_ERR_INVALID_ARG;
  if (runtime == NULL || config == NULL || config->prepare == NULL ||
      config->alipay == NULL || config->alipay->vtable == NULL)
    return result->result;
  const h2_alipay_api_t* api = config->alipay;
  const h2_alipay_vtable_t* vtable = api->vtable;
  if (vtable->pre_init == NULL || vtable->get_binding_code == NULL) {
    result->result = H2_PAL_ERR_UNSUPPORTED;
    return result->result;
  }
  if (config->complete_binding && (vtable->binding_snapshot == NULL ||
                                   vtable->query_binding_result == NULL)) {
    result->result = H2_PAL_ERR_UNSUPPORTED;
    return result->result;
  }
  result->prepare_rc = config->prepare(runtime, config->user);
  result->result = result->prepare_rc;
  if (result->result != H2_PAL_OK) return result->result;
  result->stage = H2_ALIPAY_E2E_PRE_INIT;
  uint64_t started = now(runtime);
  result->init_rc =
      vtable->pre_init(api->user, &result->init_sdk_rc, &result->first_init);
  result->init_ms = now(runtime) - started;
  result->result = result->init_rc;
  if (result->result != H2_PAL_OK) return result->result;
  if (result->init_sdk_rc != 0) {
    result->result = H2_PAL_ERR_IO;
    return result->result;
  }
  result->stage = H2_ALIPAY_E2E_REENTRY;
  bool first = true;
  result->reentry_rc =
      vtable->pre_init(api->user, &result->reentry_sdk_rc, &first);
  result->reused = result->reentry_rc == H2_PAL_OK && !first &&
                   result->reentry_sdk_rc == result->init_sdk_rc;
  result->result = result->reentry_rc;
  if (result->result != H2_PAL_OK) return result->result;
  if (!result->reused) {
    result->result = H2_PAL_ERR_INVALID_STATE;
    return result->result;
  }
  if (config->complete_binding) {
    result->stage = H2_ALIPAY_E2E_PREREQUISITES;
    result->result = vtable->binding_snapshot(api->user, &result->binding);
    if (result->result != H2_PAL_OK) return result->result;
    if (result->binding.state == H2_ALIPAY_BINDING_COMPLETE) {
      result->stage = H2_ALIPAY_E2E_BINDING_COMPLETE;
      return H2_PAL_OK;
    }
    if (result->binding.state != H2_ALIPAY_BINDING_UNBOUND) {
      result->result = H2_PAL_ERR_INVALID_STATE;
      return result->result;
    }
  }
  return request_code(runtime, config, result);
}

h2_pal_result_t h2_alipay_e2e_complete_binding(
    h2_runtime_t* runtime, const h2_alipay_e2e_config_t* config,
    bool (*cancel)(void*), void* cancel_user, h2_alipay_e2e_result_t* result) {
  if (runtime == NULL || config == NULL || result == NULL ||
      !config->complete_binding || config->alipay == NULL ||
      config->alipay->vtable == NULL ||
      config->alipay->vtable->query_binding_result == NULL ||
      result->stage != H2_ALIPAY_E2E_CODE_READY ||
      result->result != H2_PAL_OK || !result->code_presented)
    return H2_PAL_ERR_INVALID_STATE;
  uint64_t started, current;
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  if (rc != H2_PAL_OK) {
    result->result = rc;
    return rc;
  }
  uint32_t timeout = config->binding_timeout_ms;
  uint32_t interval =
      config->binding_interval_ms != 0u ? config->binding_interval_ms : 2000u;
  result->stage = H2_ALIPAY_E2E_WAIT_BINDING;
  for (;;) {
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &current);
    if (rc != H2_PAL_OK) break;
    result->binding_ms = current - started;
    if (cancel != NULL && cancel(cancel_user)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    if (timeout != 0u && result->binding_ms >= timeout) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    ++result->binding_queries;
    rc = config->alipay->vtable->query_binding_result(config->alipay->user,
                                                      &result->binding);
    result->result = rc;
    if (config->binding_progress != NULL)
      config->binding_progress(runtime, config->user, result);
    if (rc != H2_PAL_OK) break;
    if (result->binding.state == H2_ALIPAY_BINDING_COMPLETE) {
      result->stage = H2_ALIPAY_E2E_BINDING_COMPLETE;
      break;
    }
    if (result->binding.state != H2_ALIPAY_BINDING_PENDING &&
        result->binding.state != H2_ALIPAY_BINDING_STARTED) {
      rc = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &current);
    if (rc != H2_PAL_OK) break;
    uint64_t elapsed = current - started;
    if (timeout != 0u && elapsed >= timeout) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    uint64_t remaining = timeout != 0u ? timeout - elapsed : interval;
    rc = h2_pal_time_sleep_ms(
        runtime->time, remaining < interval ? (uint32_t)remaining : interval);
    if (rc != H2_PAL_OK) break;
  }
  if (h2_pal_time_get_monotonic_ms(runtime->time, &current) == H2_PAL_OK)
    result->binding_ms = current - started;
  result->result = rc;
  return rc;
}

h2_pal_result_t h2_alipay_e2e_refresh_binding(
    h2_runtime_t* runtime, const h2_alipay_e2e_config_t* config,
    bool (*cancel)(void*), void* cancel_user, h2_alipay_e2e_result_t* result) {
  if (runtime == NULL || config == NULL || result == NULL ||
      !config->complete_binding || config->alipay == NULL ||
      config->alipay->vtable == NULL ||
      config->alipay->vtable->binding_snapshot == NULL ||
      config->alipay->vtable->get_binding_code == NULL ||
      result->binding.state != H2_ALIPAY_BINDING_FAILED)
    return H2_PAL_ERR_INVALID_STATE;
  if (cancel != NULL && cancel(cancel_user))
    return result->result = H2_PAL_ERR_CLOSED;
  result->stage = H2_ALIPAY_E2E_PREREQUISITES;
  result->result = config->alipay->vtable->binding_snapshot(
      config->alipay->user, &result->binding);
  if (result->result != H2_PAL_OK) return result->result;
  if (result->binding.state != H2_ALIPAY_BINDING_UNBOUND)
    return result->result = H2_PAL_ERR_INVALID_STATE;
  uint32_t interval =
      config->binding_interval_ms != 0u ? config->binding_interval_ms : 2000u;
  result->result = h2_pal_time_sleep_ms(runtime->time, interval);
  if (result->result != H2_PAL_OK) return result->result;
  if (cancel != NULL && cancel(cancel_user))
    return result->result = H2_PAL_ERR_CLOSED;
  result->binding_queries = 0u;
  result->binding_ms = 0u;
  return request_code(runtime, config, result);
}
