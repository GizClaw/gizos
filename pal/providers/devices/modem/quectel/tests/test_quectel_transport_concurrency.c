#include "h2_quectel_modem.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex {
  pthread_mutex_t mutex;
};
static h2_pal_result_t create_mutex(void* user,
                                    const h2_pal_mutex_config_t* cfg,
                                    h2_pal_mutex_t** out) {
  (void)user;
  (void)cfg;
  *out = calloc(1, sizeof(**out));
  assert(*out);
  pthread_mutexattr_t attr;
  assert(!pthread_mutexattr_init(&attr));
  assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
  assert(!pthread_mutex_init(&(*out)->mutex, &attr));
  assert(!pthread_mutexattr_destroy(&attr));
  return H2_PAL_OK;
}
static h2_pal_result_t destroy_mutex(void* user, h2_pal_mutex_t* m) {
  (void)user;
  assert(!pthread_mutex_destroy(&m->mutex));
  free(m);
  return H2_PAL_OK;
}
static h2_pal_result_t lock_mutex(void* user, h2_pal_mutex_t* m) {
  (void)user;
  assert(!pthread_mutex_lock(&m->mutex));
  return H2_PAL_OK;
}
static h2_pal_result_t unlock_mutex(void* user, h2_pal_mutex_t* m) {
  (void)user;
  assert(!pthread_mutex_unlock(&m->mutex));
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex,
    .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex,
    .unlock_mutex = unlock_mutex,
};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int entered, release_callback, close_started, close_done, invalidations;
static h2_quectel_modem_t modem;
static void invalidate(void* user) {
  (void)user;
  assert(!pthread_mutex_lock(&gate));
  invalidations++;
  entered = 1;
  assert(!pthread_cond_broadcast(&cv));
  while (!release_callback) assert(!pthread_cond_wait(&cv, &gate));
  assert(!pthread_mutex_unlock(&gate));
}
static h2_pal_result_t command(void* user, const char* cmd, char* response,
                               size_t capacity, uint32_t timeout) {
  (void)user;
  (void)cmd;
  (void)timeout;
  if (capacity >= 5) memcpy(response, "OK\r\n", 5);
  return H2_PAL_OK;
}
static void* urc(void* unused) {
  (void)unused;
  h2_quectel_handle_urc_line(&modem, "+QSIMSTAT: 1,0");
  return NULL;
}
static void* close_transport(void* unused) {
  (void)unused;
  assert(!pthread_mutex_lock(&gate));
  close_started = 1;
  assert(!pthread_cond_broadcast(&cv));
  assert(!pthread_mutex_unlock(&gate));
  assert(lock_mutex(NULL, modem.operation_lock) == H2_PAL_OK);
  assert(h2_quectel_modem_transport_closing(&modem) == H2_PAL_OK);
  assert(unlock_mutex(NULL, modem.operation_lock) == H2_PAL_OK);
  assert(!pthread_mutex_lock(&gate));
  close_done = 1;
  assert(!pthread_cond_broadcast(&cv));
  assert(!pthread_mutex_unlock(&gate));
  return NULL;
}
int main(void) {
  const h2_quectel_modem_config_t config = {
      .command = command,
      .sync_api = &sync_api,
      .invalidate_data = invalidate,
  };
  assert(h2_quectel_modem_init(&modem, &config) == H2_PAL_OK);
  modem.opened = 1u;
  modem.sim_presence = 1u;
  pthread_t receiver, closer;
  assert(!pthread_create(&receiver, NULL, urc, NULL));
  assert(!pthread_mutex_lock(&gate));
  while (!entered) assert(!pthread_cond_wait(&cv, &gate));
  assert(!pthread_mutex_unlock(&gate));
  assert(!pthread_create(&closer, NULL, close_transport, NULL));
  assert(!pthread_mutex_lock(&gate));
  while (!close_started) assert(!pthread_cond_wait(&cv, &gate));
  assert(!close_done);
  release_callback = 1;
  assert(!pthread_cond_broadcast(&cv));
  assert(!pthread_mutex_unlock(&gate));
  assert(!pthread_join(receiver, NULL));
  assert(!pthread_join(closer, NULL));
  h2_quectel_handle_urc_line(&modem, "RDY");
  assert(invalidations == 1 && close_done && modem.transport_closed);
  /* A notification already queued behind the state lock observes the closing
   * gate rather than touching transport resources after their owner proceeds.
   */
  assert(lock_mutex(NULL, modem.operation_lock) == H2_PAL_OK);
  assert(lock_mutex(NULL, modem.lock) == H2_PAL_OK);
  modem.transport_closed = 0u;
  modem.sim_presence = 1u;
  modem.sim_state = H2_PAL_MODEM_SIM_STATE_READY;
  assert(!pthread_create(&receiver, NULL, urc, NULL));
  assert(h2_quectel_modem_transport_closing(&modem) == H2_PAL_OK);
  assert(unlock_mutex(NULL, modem.lock) == H2_PAL_OK);
  assert(unlock_mutex(NULL, modem.operation_lock) == H2_PAL_OK);
  assert(!pthread_join(receiver, NULL));
  assert(invalidations == 1);
  assert(h2_quectel_modem_transport_closed(&modem) == H2_PAL_OK);
  assert(h2_quectel_modem_deinit(&modem) == H2_PAL_OK);
  return 0;
}
