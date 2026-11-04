#include "h2_device_activity.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

static h2_pal_wifi_activity_fn observed;
static void* observed_user;
static atomic_bool entered, released;
static unsigned calls, waits;
static int blocking;
static int set_observer(void* user, h2_pal_wifi_activity_fn callback,
                        void* context) {
  (void)user;
  if (callback == NULL && observed != NULL) {
    /* Already captured by an SDK call, even though unregister has begun. */
    observed(observed_user, false);
  }
  observed = callback;
  observed_user = context;
  return H2_PAL_OK;
}
static void on_activity(void* user, bool active) {
  assert(user == &calls);
  ++calls;
  if (blocking && active) {
    atomic_store(&entered, true);
    while (!atomic_load(&released)) sched_yield();
  }
}
static int sleep_ms(void* user, uint32_t delay) {
  (void)user;
  assert(delay == 1u);
  ++waits;
  atomic_store(&released, true);
  sched_yield();
  return H2_PAL_OK;
}
static void* invoke(void* ignored) {
  (void)ignored;
  observed(observed_user, true);
  return NULL;
}
int main(void) {
  const h2_pal_wifi_sta_vtable_t wifi_ops = {.set_activity_observer =
                                                 set_observer};
  const h2_pal_wifi_sta_api_t wifi = {.vtable = &wifi_ops};
  const h2_pal_time_vtable_t time_ops = {.sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {.vtable = &time_ops};
  h2_device_activity_t* first = malloc(sizeof(*first));
  assert(first != NULL);
  assert(h2_device_activity_bind(first, &wifi, &time, on_activity, &calls) ==
         0);
  h2_device_activity_t other;
  assert(h2_device_activity_bind(&other, &wifi, &time, on_activity, &calls) ==
         H2_PAL_ERR_BUSY);
  h2_pal_wifi_activity_fn stale = observed;
  void* stale_user = observed_user;
  blocking = 1;
  pthread_t worker;
  assert(pthread_create(&worker, NULL, invoke, NULL) == 0);
  while (!atomic_load(&entered)) sched_yield();
  h2_device_activity_unbind(first);
  assert(pthread_join(worker, NULL) == 0);
  assert(calls == 1u && waits != 0u);
  free(first);
  stale(stale_user, true);
  assert(calls == 1u);
  blocking = 0;
  assert(h2_device_activity_bind(&other, &wifi, &time, on_activity, &calls) ==
         0);
  stale(stale_user, true);
  assert(calls == 1u);
  observed(observed_user, false);
  assert(calls == 2u);
  h2_device_activity_unbind(&other);
  h2_device_activity_unbind(&other);
  assert(calls == 2u);
  return 0;
}
