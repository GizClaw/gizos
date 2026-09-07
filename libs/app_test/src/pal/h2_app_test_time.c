#include "h2_app_test_time.h"
#include <string.h>

h2_pal_result_t h2_app_test_time_advance(h2_app_test_time_t *c,
                                         uint64_t delta) {
  if (!c)
    return H2_PAL_ERR_INVALID_ARG;
  if (delta > UINT64_MAX - c->monotonic_ms ||
      (c->wall_status.valid && delta > UINT64_MAX - c->wall_ms))
    return H2_PAL_ERR_NO_SPACE;
  c->monotonic_ms += delta;
  if (c->wall_status.valid)
    c->wall_ms += delta;
  return H2_PAL_OK;
}
static h2_pal_result_t now(void *u, uint64_t *out) {
  h2_app_test_time_t *c = u;
  *out = 0;
  int rc = h2_app_test_fault_take(&c->read);
  if (!rc) {
    *out = c->monotonic_ms;
    rc = h2_app_test_time_advance(c, c->advance_per_read_ms);
    if (rc) *out = 0u;
  }
  return rc;
}
static h2_pal_result_t micros(void *u, uint64_t *out) {
  uint64_t ms = 0;
  *out = 0;
  int rc = now(u, &ms);
  if (rc)
    return rc;
  if (ms > UINT64_MAX / 1000u)
    return H2_PAL_ERR_NO_SPACE;
  *out = ms * 1000u;
  return H2_PAL_OK;
}
static h2_pal_result_t wall(void *u, uint64_t *out) {
  h2_app_test_time_t *c = u;
  *out = 0;
  int rc = h2_app_test_fault_take(&c->read);
  if (rc)
    return rc;
  if (!c->wall_status.valid)
    return H2_PAL_ERR_UNAVAILABLE;
  *out = c->wall_ms;
  return H2_PAL_OK;
}
static h2_pal_result_t set_wall(void *u, uint64_t value) {
  h2_app_test_time_t *c = u;
  int rc = h2_app_test_fault_take(&c->set_wall);
  if (!rc) {
    c->wall_ms = value;
    c->wall_status =
        (h2_pal_time_wall_status_t){1, H2_PAL_TIME_WALL_SOURCE_USER};
  }
  return rc;
}
static h2_pal_result_t status(void *u, h2_pal_time_wall_status_t *out) {
  *out = ((h2_app_test_time_t *)u)->wall_status;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *u, uint32_t ms) {
  h2_app_test_time_t *c = u;
  c->last_sleep_ms = ms;
  int rc = h2_app_test_fault_take(&c->sleep);
  if (!rc) rc = h2_app_test_time_advance(c, ms);
  if (!rc && c->on_sleep) c->on_sleep(c->sleep_user, ms);
  return rc;
}
static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms = now,
                                            .get_monotonic_us = micros,
                                            .get_wall_ms = wall,
                                            .set_wall_ms = set_wall,
                                            .get_wall_status = status,
                                            .sleep_ms = sleep_ms};
void h2_app_test_time_init(h2_app_test_time_t *c, uint64_t ms) {
  if (!c)
    return;
  memset(c, 0, sizeof(*c));
  c->monotonic_ms = ms;
  c->api = (h2_pal_time_api_t){c, &vtable};
}
