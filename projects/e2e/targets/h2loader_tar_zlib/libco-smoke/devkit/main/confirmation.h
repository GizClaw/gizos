#ifndef H2_LIBCO_SMOKE_CONFIRMATION_H
#define H2_LIBCO_SMOKE_CONFIRMATION_H

#include "h2_loader_app_client.h"
#include "h2_runtime.h"

/* Borrow the command service's lock; confirmation and release stay on root. */
static inline int h2_libco_smoke_confirm(
    h2_runtime_t *runtime,
    int (*get_config)(h2_loader_app_client_config_t *),
    int (*confirm)(h2_runtime_t *)) {
  h2_loader_app_client_config_t config = {0};
  if (runtime == NULL || get_config == NULL || confirm == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = get_config(&config);
  if (result != H2_PAL_OK) return result;
  if (config.operation_mutex == NULL || config.operation_sync == NULL ||
      config.operation_sync->vtable == NULL ||
      config.operation_sync->vtable->lock_mutex == NULL ||
      config.operation_sync->vtable->unlock_mutex == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  result = h2_pal_mutex_lock(config.operation_sync, config.operation_mutex);
  if (result != H2_PAL_OK) return result;
  result = confirm(runtime);
  const int unlock_result =
      h2_pal_mutex_unlock(config.operation_sync, config.operation_mutex);
  return result != H2_PAL_OK ? result : unlock_result;
}

#endif
