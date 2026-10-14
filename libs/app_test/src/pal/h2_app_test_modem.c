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
static h2_pal_result_t emergency_numbers(void *u, uint32_t ms,
    h2_pal_modem_emergency_number_t *out, size_t capacity, size_t *out_count) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_emergency_numbers_timeout_ms = ms;
  h2_pal_result_t rc = h2_app_test_fault_take(&m->get_emergency_numbers);
  if (rc != H2_PAL_OK)
    return rc;
  if (m->emergency_number_count > capacity)
    return H2_PAL_ERR_TRUNCATED;
  if (m->emergency_number_count != 0u) {
    if (m->emergency_numbers == NULL)
      return H2_PAL_ERR_INVALID_STATE;
    memcpy(out, m->emergency_numbers, m->emergency_number_count * sizeof(*out));
  }
  *out_count = m->emergency_number_count;
  return H2_PAL_OK;
}
static h2_pal_result_t identity(void *u, h2_pal_modem_identity_t *out) {
  h2_app_test_modem_t *m = u;
  memset(out, 0, sizeof(*out));
  h2_pal_result_t rc = h2_app_test_fault_take(&m->get_identity);
  if (rc == H2_PAL_OK)
    *out = m->identity;
  return rc;
}
static h2_pal_result_t ota_start(void *u, const h2_pal_modem_ota_request_t *request) {
  h2_app_test_modem_t *m = u;
  if (request == NULL || request->url == NULL ||
      !h2_pal_modem_ota_revision_valid(request->target_revision) ||
      (request->expected_revision != NULL && !h2_pal_modem_ota_revision_valid(request->expected_revision)))
    return H2_PAL_ERR_INVALID_ARG;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_OTA))
    return H2_PAL_ERR_UNSUPPORTED;
  size_t length = 0u;
  while (length < sizeof(m->last_ota_url) && request->url[length] != '\0')
    length++;
  if (length == sizeof(m->last_ota_url))
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(m->last_ota_url, request->url, length + 1u);
  strcpy(m->last_ota_target_revision, request->target_revision);
  m->last_ota_expected_revision[0] = '\0';
  if (request->expected_revision != NULL)
    strcpy(m->last_ota_expected_revision, request->expected_revision);
  m->last_ota_request = *request;
  m->last_ota_request.url = m->last_ota_url;
  m->last_ota_request.target_revision = m->last_ota_target_revision;
  m->last_ota_request.expected_revision = request->expected_revision != NULL
      ? m->last_ota_expected_revision : NULL;
  return h2_app_test_fault_take(&m->ota_start);
}
static h2_pal_result_t ota_get_status(void *u, uint32_t timeout_ms, h2_pal_modem_ota_status_t *out) {
  h2_app_test_modem_t *m = u;
  if (!(m->status.capabilities & H2_PAL_MODEM_CAPABILITY_OTA))
    return H2_PAL_ERR_UNSUPPORTED;
  m->last_ota_status_timeout_ms = timeout_ms;
  h2_pal_result_t rc = h2_app_test_fault_take(&m->ota_get_status);
  if (rc == H2_PAL_OK)
    *out = m->ota_status;
  return rc;
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
                                             .get_identity = identity,
                                             .ota_start = ota_start,
                                             .ota_get_status = ota_get_status,
                                             .get_emergency_numbers = emergency_numbers,
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
