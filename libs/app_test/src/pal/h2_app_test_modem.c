#include "h2_app_test_modem.h"
#include <string.h>
static int caps(void *u, uint32_t *out) {
  *out = ((h2_app_test_modem_t *)u)->status.capabilities;
  return 0;
}
static int status(void *u, h2_pal_modem_status_t *out) {
  h2_app_test_modem_t *m = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&m->get_status);
  if (!rc)
    *out = m->status;
  return rc;
}
static int dial(void *u, const h2_pal_modem_call_request_t *r) {
  h2_app_test_modem_t *m = u;
  if (!r || !r->number[0] || !memchr(r->number, 0, sizeof(r->number)))
    return H2_PAL_ERR_INVALID_ARG;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_dial = *r;
  return h2_app_test_fault_take(&m->dial);
}
static int answer(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_answer_timeout_ms = ms;
  return h2_app_test_fault_take(&m->answer);
}
static int hangup(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_hangup_timeout_ms = ms;
  return h2_app_test_fault_take(&m->hangup);
}
static const h2_pal_modem_vtable_t vtable = {.get_capabilities = caps,
                                             .get_status = status,
                                             .call_dial = dial,
                                             .call_answer = answer,
                                             .call_hangup = hangup};
void h2_app_test_modem_init(h2_app_test_modem_t *m) {
  if (!m)
    return;
  memset(m, 0, sizeof(*m));
  m->api = (h2_pal_modem_api_t){m, &vtable};
  m->status.capabilities = H2_PAL_MODEM_CAPABILITY_CALL;
}
