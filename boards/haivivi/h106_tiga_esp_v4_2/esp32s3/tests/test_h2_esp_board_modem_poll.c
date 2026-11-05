#include "h2_esp_board_modem_poll.h"
#include <assert.h>

struct h2_pal_mutex {
  int held;
};
static int polls, unlocks;
static h2_pal_result_t poll_rc, unlock_rc;
static h2_pal_result_t try_lock(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  if (mutex->held) return H2_PAL_ERR_BUSY;
  mutex->held = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t unlock(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex->held);
  mutex->held = 0;
  ++unlocks;
  return unlock_rc;
}
static h2_pal_result_t poll(void* user, h2_pal_modem_call_status_t* status) {
  (void)status;
  assert(((h2_pal_mutex_t*)user)->held);
  ++polls;
  return poll_rc;
}
int main(void) {
  h2_pal_mutex_t lock = {.held = 1};
  const h2_pal_sync_vtable_t sync_vtable = {.try_lock_mutex = try_lock,
                                            .unlock_mutex = unlock};
  const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  const h2_pal_modem_vtable_t modem_vtable = {.get_call_status = poll};
  h2_pal_modem_t modem = {.user = &lock, .vtable = &modem_vtable};
  h2_pal_modem_call_status_t status = {0};
  /* Closing holds the lock and waits for STOP: poll must not block or query. */
  assert(h2_tiga_modem_poll_call(&sync, &lock, &modem, &status) ==
         H2_PAL_ERR_BUSY);
  assert(polls == 0 && unlocks == 0 && lock.held);
  lock.held = 0;
  assert(h2_tiga_modem_poll_call(&sync, &lock, &modem, &status) == H2_PAL_OK);
  assert(polls == 1 && unlocks == 1 && !lock.held);
  poll_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_tiga_modem_poll_call(&sync, &lock, &modem, &status) ==
         H2_PAL_ERR_TIMEOUT);
  assert(unlocks == 2 && !lock.held);
  poll_rc = H2_PAL_OK;
  unlock_rc = H2_PAL_ERR_IO;
  assert(h2_tiga_modem_poll_call(&sync, &lock, &modem, &status) ==
         H2_PAL_ERR_IO);
  return 0;
}
