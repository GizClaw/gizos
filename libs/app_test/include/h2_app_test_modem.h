#ifndef H2_APP_TEST_MODEM_H
#define H2_APP_TEST_MODEM_H
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2_app_test_fault.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Scripted Modem boundary. Status and emergency tables are supplied by the scenario; successful
 * dial/answer/hangup records intent without synthesizing asynchronous events.
 */
typedef struct h2_app_test_modem {
  h2_pal_modem_api_t api;
  h2_pal_modem_status_t status;
  h2_pal_modem_identity_t identity;
  h2_pal_modem_ota_status_t ota_status;
  /** Recorded intent only; the scenario supplies identity/OTA status separately. */
  h2_pal_modem_ota_request_t last_ota_request;
  char last_ota_url[256];
  char last_ota_expected_revision[H2_PAL_MODEM_IDENTITY_MAX];
  char last_ota_target_revision[H2_PAL_MODEM_IDENTITY_MAX];
  uint32_t last_ota_status_timeout_ms;
  h2_app_test_fault_t get_identity, ota_start, ota_get_status;
  h2_pal_modem_call_request_t last_dial;
  uint32_t last_answer_timeout_ms, last_hangup_timeout_ms;
  /** Borrowed scenario entries; retain storage until the last query finishes.
   * Enable EMERGENCY_NUMBERS in status.capabilities to expose this fixture. */
  const h2_pal_modem_emergency_number_t *emergency_numbers;
  size_t emergency_number_count;
  uint32_t last_emergency_numbers_timeout_ms;
  h2_app_test_fault_t get_emergency_numbers;
  h2_app_test_fault_t get_status, dial, answer, hangup;
  /** Opt-in scripted lifecycle. When false open/close return UNSUPPORTED.
   * Successful calls only record intent; they never operate physical power.
   * Failures retain opened. Configure while quiescent, then serialize calls. */
  bool lifecycle_supported, opened;
  uint32_t last_open_timeout_ms, last_close_timeout_ms;
  h2_app_test_fault_t open, close;
} h2_app_test_modem_t;
/** Initialize caller storage with CALL capability; status is otherwise unknown.
 */
void h2_app_test_modem_init(h2_app_test_modem_t *modem);

#ifdef __cplusplus
}
#endif
#endif
