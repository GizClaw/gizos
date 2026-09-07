#ifndef H2_APP_TEST_MODEM_H
#define H2_APP_TEST_MODEM_H
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Scripted Modem call boundary. Status is supplied by the scenario; successful
 * dial/answer/hangup records intent without synthesizing asynchronous events.
 */
typedef struct h2_app_test_modem {
  h2_pal_modem_api_t api;
  h2_pal_modem_status_t status;
  h2_pal_modem_call_request_t last_dial;
  uint32_t last_answer_timeout_ms, last_hangup_timeout_ms;
  h2_app_test_fault_t get_status, dial, answer, hangup;
} h2_app_test_modem_t;
/** Initialize caller storage with CALL capability; status is otherwise unknown.
 */
void h2_app_test_modem_init(h2_app_test_modem_t *modem);

#ifdef __cplusplus
}
#endif
#endif
