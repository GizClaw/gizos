#include "h2_app_test_power.h"
#include <string.h>
static int caps(void *u, h2_pal_power_capabilities_t *o) {
  *o = ((h2_app_test_power_t *)u)->capabilities;
  return 0;
}
static int boot(void *u, h2_pal_power_boot_info_t *o) {
  *o = ((h2_app_test_power_t *)u)->boot_info;
  return 0;
}
static int state(void *u, h2_pal_power_state_t *o) {
  *o = ((h2_app_test_power_t *)u)->state;
  return 0;
}
static int hold(void *u, int enabled) {
  h2_app_test_power_t *p = u;
  if (!(p->capabilities.flags & H2_PAL_POWER_CAPABILITY_HOLD))
    return H2_PAL_ERR_UNSUPPORTED;
  int rc = h2_app_test_fault_take(&p->hold);
  if (!rc)
    p->hold_enabled = !!enabled;
  return rc;
}
static int get_hold(void *u, h2_pal_power_hold_state_t *o) {
  o->enabled = ((h2_app_test_power_t *)u)->hold_enabled;
  return 0;
}
static int reboot(void *u, uint32_t reason) {
  h2_app_test_power_t *p = u;
  if (!(p->capabilities.flags & H2_PAL_POWER_CAPABILITY_REBOOT))
    return H2_PAL_ERR_UNSUPPORTED;
  p->last_reason = reason;
  int rc = h2_app_test_fault_take(&p->reboot);
  if (!rc)
    p->state = H2_PAL_POWER_STATE_REBOOTING;
  return rc;
}
static int shutdown(void *u, uint32_t reason) {
  h2_app_test_power_t *p = u;
  if (!(p->capabilities.flags & H2_PAL_POWER_CAPABILITY_SHUTDOWN))
    return H2_PAL_ERR_UNSUPPORTED;
  p->last_reason = reason;
  int rc = h2_app_test_fault_take(&p->shutdown);
  if (!rc)
    p->state = H2_PAL_POWER_STATE_OFF;
  return rc;
}
static int sleep(void *u, uint32_t reason) {
  h2_app_test_power_t *p = u;
  if (!(p->capabilities.flags & H2_PAL_POWER_CAPABILITY_SLEEP))
    return H2_PAL_ERR_UNSUPPORTED;
  p->last_reason = reason;
  int rc = h2_app_test_fault_take(&p->sleep);
  if (!rc)
    p->state = H2_PAL_POWER_STATE_SLEEPING;
  return rc;
}
static int deep_sleep(void *u, uint32_t reason) {
  h2_app_test_power_t *p = u;
  if (!(p->capabilities.flags & H2_PAL_POWER_CAPABILITY_DEEP_SLEEP))
    return H2_PAL_ERR_UNSUPPORTED;
  p->last_reason = reason;
  int rc = h2_app_test_fault_take(&p->deep_sleep);
  if (!rc)
    p->state = H2_PAL_POWER_STATE_DEEP_SLEEPING;
  return rc;
}
static const h2_pal_power_vtable_t vtable = {.get_capabilities = caps,
                                             .get_boot_info = boot,
                                             .get_state = state,
                                             .set_hold = hold,
                                             .get_hold = get_hold,
                                             .reboot = reboot,
                                             .shutdown = shutdown,
                                             .sleep = sleep,
                                             .deep_sleep = deep_sleep};
void h2_app_test_power_init(h2_app_test_power_t *p) {
  if (!p)
    return;
  memset(p, 0, sizeof(*p));
  p->api = (h2_pal_power_api_t){p, &vtable};
  p->state = H2_PAL_POWER_STATE_RUNNING;
  p->capabilities.flags =
      H2_PAL_POWER_CAPABILITY_HOLD | H2_PAL_POWER_CAPABILITY_REBOOT |
      H2_PAL_POWER_CAPABILITY_SHUTDOWN | H2_PAL_POWER_CAPABILITY_SLEEP |
      H2_PAL_POWER_CAPABILITY_DEEP_SLEEP;
}
