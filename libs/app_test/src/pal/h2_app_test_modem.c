#include "h2_app_test_modem.h"
#include <string.h>
static h2_pal_result_t caps(void *u, uint32_t *out) {
  *out = ((h2_app_test_modem_t *)u)->status.capabilities;
  return 0;
}
static h2_pal_result_t status(void *u, h2_pal_modem_status_t *out) {
  h2_app_test_modem_t *m = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&m->get_status);
  if (!rc)
    *out = m->status;
  return rc;
}
static h2_pal_result_t dial(void *u, const h2_pal_modem_call_request_t *r) {
  h2_app_test_modem_t *m = u;
  if (!r || !r->number[0] || !memchr(r->number, 0, sizeof(r->number)))
    return H2_PAL_ERR_INVALID_ARG;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_dial = *r;
  return h2_app_test_fault_take(&m->dial);
}
static h2_pal_result_t answer(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_answer_timeout_ms = ms;
  return h2_app_test_fault_take(&m->answer);
}
static h2_pal_result_t hangup(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_CALL))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_hangup_timeout_ms = ms;
  return h2_app_test_fault_take(&m->hangup);
}
static h2_pal_result_t open_modem(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!m->lifecycle_supported) return H2_PAL_ERR_UNSUPPORTED;
  m->last_open_timeout_ms = ms;
  if (m->opened) return H2_PAL_OK;
  int rc = h2_app_test_fault_take(&m->open);
  if (rc == H2_PAL_OK) m->opened = true;
  return rc;
}
static h2_pal_result_t close_modem(void *u, uint32_t ms) {
  h2_app_test_modem_t *m = u;
  if (!m->lifecycle_supported) return H2_PAL_ERR_UNSUPPORTED;
  m->last_close_timeout_ms = ms;
  int rc = h2_app_test_fault_take(&m->close);
  if (rc == H2_PAL_OK) m->opened = false;
  return rc;
}
static const h2_pal_modem_vtable_t vtable = {.get_capabilities = caps,
                                             .get_status = status,
                                             .call_dial = dial,
                                             .call_answer = answer,
                                             .call_hangup = hangup,
                                             .open = open_modem,
                                             .close = close_modem};
void h2_app_test_modem_init(h2_app_test_modem_t *m) {
  if (!m)
    return;
  memset(m, 0, sizeof(*m));
  m->api = (h2_pal_modem_api_t){m, &vtable};
  m->status.capabilities = H2_PAL_MODEM_CAPABILITY_CALL;
}
