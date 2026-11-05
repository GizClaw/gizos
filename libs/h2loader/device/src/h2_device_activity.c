#include "h2_device_activity.h"
#include "h2_atomic_static.h"

#include <limits.h>
#include <stdint.h>

/* Native Wi-Fi calls may retain callback/user after unregister returns. Only
 * this static mailbox, never caller-owned state, is reachable through that API.
 */
H2_ATOMIC_DEFINE_STATIC(ptr, owner, NULL);
H2_ATOMIC_DEFINE_STATIC(bool, enabled, false);
H2_ATOMIC_DEFINE_STATIC(uint, readers, 0u);
H2_ATOMIC_DEFINE_STATIC(uint, generation, 0u);

static void activity(void* user, bool active) {
  unsigned token = (unsigned)(uintptr_t)user;
  if (!h2_atomic_bool_load(&enabled, H2_ATOMIC_SEQ_CST) ||
      h2_atomic_uint_load(&generation, H2_ATOMIC_SEQ_CST) != token)
    return;
  h2_atomic_uint_fetch_add(&readers, 1u, H2_ATOMIC_SEQ_CST);
  if (h2_atomic_bool_load(&enabled, H2_ATOMIC_SEQ_CST) &&
      h2_atomic_uint_load(&generation, H2_ATOMIC_SEQ_CST) == token) {
    h2_device_activity_t* binding =
        h2_atomic_ptr_load(&owner, H2_ATOMIC_SEQ_CST);
    if (binding != NULL) binding->callback(binding->user, active);
  }
  h2_atomic_uint_fetch_sub(&readers, 1u, H2_ATOMIC_SEQ_CST);
}

int h2_device_activity_bind(h2_device_activity_t* binding,
                            const h2_pal_wifi_sta_api_t* wifi,
                            const h2_pal_time_api_t* time,
                            h2_pal_wifi_activity_fn callback, void* user) {
  if (binding == NULL || wifi == NULL || time == NULL || callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_atomic_uint_load(&generation, H2_ATOMIC_SEQ_CST) == UINT_MAX)
    return H2_PAL_ERR_NO_SPACE;
  void* expected = NULL;
  if (!h2_atomic_ptr_compare_exchange(&owner, &expected, binding,
                                      H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST))
    return H2_PAL_ERR_BUSY;
  *binding = (h2_device_activity_t){wifi, time, callback, user};
  unsigned token =
      h2_atomic_uint_fetch_add(&generation, 1u, H2_ATOMIC_SEQ_CST) + 1u;
  h2_atomic_bool_store(&enabled, true, H2_ATOMIC_SEQ_CST);
  int rc = h2_pal_wifi_sta_set_activity_observer(wifi, activity,
                                                 (void*)(uintptr_t)token);
  if (rc != H2_PAL_OK) h2_device_activity_unbind(binding);
  return rc;
}

void h2_device_activity_unbind(h2_device_activity_t* binding) {
  if (binding == NULL ||
      h2_atomic_ptr_load(&owner, H2_ATOMIC_SEQ_CST) != binding)
    return;
  h2_atomic_bool_store(&enabled, false, H2_ATOMIC_SEQ_CST);
  (void)h2_pal_wifi_sta_set_activity_observer(binding->wifi, NULL, NULL);
  while (h2_atomic_uint_load(&readers, H2_ATOMIC_SEQ_CST) != 0u)
    (void)h2_pal_time_sleep_ms(binding->time, 1u);
  h2_atomic_ptr_store(&owner, NULL, H2_ATOMIC_SEQ_CST);
}
