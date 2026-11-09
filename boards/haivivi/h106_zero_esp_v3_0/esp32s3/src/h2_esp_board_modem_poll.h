#ifndef H2_ESP_BOARD_MODEM_POLL_H
#define H2_ESP_BOARD_MODEM_POLL_H
#include "os/h2_pal_sync.h"
#include "hal/h2_pal_modem.h"

/* Close owns the operation lock while joining the monitor. Never wait for
 * that lock here: return to the worker's STOP check when a transaction owns it.
 * The provider uses the same recursive lock for the nested status query. */
static inline h2_pal_result_t h2_zero_esp_modem_poll_call(
    const h2_pal_sync_api_t* sync, h2_pal_mutex_t* operation_lock,
    h2_pal_modem_t* modem, h2_pal_modem_call_status_t* status) {
  h2_pal_result_t rc = h2_pal_mutex_try_lock(sync, operation_lock);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_pal_modem_get_call_status(modem, status);
  const h2_pal_result_t unlock_rc = h2_pal_mutex_unlock(sync, operation_lock);
  return rc != H2_PAL_OK ? rc : unlock_rc;
}
#endif
