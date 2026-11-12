#include "h2_alipay_independent.h"

#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include <string.h>

static h2_alipay_client_config_t s_config;
static const h2_pal_sync_api_t* s_sync;
static h2_pal_mutex_t* s_lock;
static h2_alipay_client_result_t s_session;
static bool s_initialized;

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
  if (config == NULL || sync == NULL || config->mem == NULL ||
      config->time == NULL || config->http == NULL || config->pref == NULL ||
      config->crypto == NULL || config->company == NULL ||
      config->pref_namespace == NULL)
    return H2_PAL_ERR_INVALID_ARG;
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
  rc = s_initialized && s_session.length != 0u ? H2_PAL_ERR_UNSUPPORTED
                                               : H2_PAL_ERR_INVALID_STATE;
  return sdk_result(finish(rc));
}

bool alipay_get_binding_status(void) { return false; }

h2_pal_result_t h2_alipay_independent_release(void) {
  if (s_lock == NULL) return H2_PAL_OK;
  int rc = h2_pal_mutex_destroy(s_sync, s_lock);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_client_result_clear(&s_session);
  memset(&s_config, 0, sizeof(s_config));
  s_lock = NULL;
  s_sync = NULL;
  s_initialized = false;
  return H2_PAL_OK;
}
