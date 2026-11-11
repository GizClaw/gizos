#include "h2_alipay_e2e.h"
#include <string.h>

static uint64_t now(h2_runtime_t* runtime) {
  uint64_t ms = 0;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &ms);
  return ms;
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
  if (vtable->pre_init == NULL || vtable->check_ready == NULL ||
      vtable->binding_status == NULL || vtable->get_binding_code == NULL) {
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
  result->stage = H2_ALIPAY_E2E_PREREQUISITES;
  result->result =
      vtable->check_ready(api->user, config->feature, &result->blocker);
  if (result->result != H2_PAL_OK) return result->result;
  bool bound = false;
  result->result = vtable->binding_status(api->user, &bound);
  if (result->result != H2_PAL_OK) return result->result;
  if (bound) {
    result->stage = H2_ALIPAY_E2E_BOUND;
    result->result = H2_PAL_ERR_INVALID_STATE;
    return result->result;
  }
  result->stage = H2_ALIPAY_E2E_GET_CODE;
  started = now(runtime);
  result->result = vtable->get_binding_code(
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
