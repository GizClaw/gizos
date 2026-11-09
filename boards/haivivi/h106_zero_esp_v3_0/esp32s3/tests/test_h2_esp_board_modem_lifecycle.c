#include "h2_esp_board_modem_lifecycle.h"
#include <assert.h>

static int stage, effects, cuts, attempts;
static uint32_t observed_timeout;
static h2_pal_result_t lock_error, admission_error, close_error, open_error;
static h2_pal_result_t lock(uint32_t timeout) {
  observed_timeout = timeout;
  assert(stage == 0);
  if (lock_error != H2_PAL_OK) return lock_error;
  stage = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t admit(void) {
  assert(stage == 1);
  stage = 2;
  return admission_error;
}
static h2_pal_result_t close_effect(void* user, uint32_t timeout) {
  (void)user;
  assert(stage == 2 && timeout == observed_timeout);
  stage = 3;
  effects++;
  cuts++;
  return close_error;
}
static h2_pal_result_t unlock(void) {
  assert(stage == 2 || stage == 3);
  stage = 0;
  return H2_PAL_OK;
}
static h2_pal_result_t close_run(void* user, uint32_t timeout) {
  const h2_board_modem_close_ops_t ops = {lock, admit, close_effect, unlock};
  return h2_board_modem_close_run(&ops, user, timeout);
}
static h2_pal_result_t open_run(void* user, uint32_t timeout) {
  (void)user;
  observed_timeout = timeout;
  assert(timeout != 0u);
  attempts++;
  return open_error;
}
static void failed(void* user, h2_pal_result_t error) {
  (void)user;
  assert(error != H2_PAL_OK && error != H2_PAL_ERR_BUSY);
  cuts++;
}
int main(void) {
  const h2_board_modem_lifecycle_ops_t ops = {open_run, close_run, failed};
  /* Both first open and retry receive the same complete default budget before
   * deadline creation and operation-lock admission in the production adapter.
   */
  open_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 0u, true) ==
         H2_PAL_ERR_TIMEOUT);
  assert(observed_timeout == 45000u && attempts == 1 && cuts == 1);
  open_error = H2_PAL_OK;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 0u, true) == H2_PAL_OK);
  assert(observed_timeout == 45000u && attempts == 2 && cuts == 1);
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 1234u, true) == H2_PAL_OK);
  assert(observed_timeout == 1234u);
  admission_error = H2_PAL_ERR_BUSY;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 500u, false) ==
         H2_PAL_ERR_BUSY);
  assert(cuts == 1 && effects == 0 && stage == 0);
  lock_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 500u, false) ==
         H2_PAL_ERR_TIMEOUT);
  assert(cuts == 1 && effects == 0 && stage == 0);
  lock_error = admission_error = H2_PAL_OK;
  close_error = H2_PAL_ERR_IO;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 500u, false) ==
         H2_PAL_ERR_IO);
  assert(cuts == 2 && effects == 1 && stage == 0);
  open_error = H2_PAL_ERR_BUSY;
  assert(h2_board_modem_lifecycle_run(&ops, NULL, 0u, true) == H2_PAL_ERR_BUSY);
  assert(cuts == 2);
  return 0;
}
