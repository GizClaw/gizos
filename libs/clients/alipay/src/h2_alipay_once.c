#include "h2_alipay.h"

h2_pal_result_t h2_alipay_initialize_once(h2_alipay_initialization_t* state,
                                          int (*initialize)(void), int* sdk_rc,
                                          bool* first_attempt) {
  if (state == NULL || initialize == NULL || sdk_rc == NULL ||
      first_attempt == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *first_attempt = !state->attempted;
  if (!state->attempted) {
    state->attempted = true;
    state->sdk_rc = initialize();
  }
  *sdk_rc = state->sdk_rc;
  return H2_PAL_OK;
}
