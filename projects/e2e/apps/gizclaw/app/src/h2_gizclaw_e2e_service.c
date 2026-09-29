#include "h2_gizclaw_e2e_service.h"

#include <stdio.h>
#include <string.h>

#define REQUEST_TIMEOUT_MS 30000u

/* Expected rejections are contract checks, not successful API-call evidence.
 * Keep the actual return value visible without resetting a successful call
 * chain in the coverage auditor. */
static int expect_result(const char *operation, int actual, int expected) {
  const int rc = actual == expected ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  printf("H2_GIZCLAW_E2E stage=service-contract-check operation=%s "
         "actual_rc=%d expected_rc=%d result=%s rc=%d\n",
         operation, actual, expected, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  return rc;
}

/* Deterministic cancellation before submission. In-flight audio cancellation
 * is exercised by the speech/voice cases, not inferred from this check. */
static int canceled_before_do(h2_gizclaw_e2e_fixture_t *fixture,
                              h2_gizclaw_service_t *service) {
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, REQUEST_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_req_t *request = NULL;
  int rc =
      h2_gizclaw_req_create_ping(service, 2u, REQUEST_TIMEOUT_MS, &request);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_create_ping", "service-cancel-new",
                          rc);
  for (unsigned i = 0u; rc == H2_PAL_OK && i < 2u; ++i) {
    rc = h2_gizclaw_req_cancel(request);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_cancel", "service-cancel-new", rc);
  }
  for (unsigned i = 0u; rc == H2_PAL_OK && i < 2u; ++i) {
    const int wait_rc = h2_gizclaw_req_wait(request, 1u);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait",
                            "service-cancel-expect-closed", wait_rc);
    if (wait_rc != H2_PAL_ERR_CLOSED)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    h2_gizclaw_ping_result_t response, empty;
    memset(&response, 0xA5, sizeof(response));
    memset(&empty, 0, sizeof(empty));
    const int parse_rc = h2_gizclaw_resp_parse_ping(request, &response);
    h2_gizclaw_e2e_evidence("h2_gizclaw_resp_parse_ping",
                            "service-cancel-expect-closed", parse_rc);
    if (parse_rc != H2_PAL_ERR_CLOSED ||
        memcmp(&response, &empty, sizeof(response)) != 0)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    const int do_rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_do",
                            "service-cancel-expect-rejected", do_rc);
    if (do_rc != H2_PAL_ERR_INVALID_STATE)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc != H2_PAL_OK && request != NULL)
    (void)h2_gizclaw_req_cancel(request);
  h2_gizclaw_req_release(request);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_release", "service", H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_release", "service-cancel-new",
                          H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_cancel", "req_cancel-assert", rc);
  return rc;
}

/* Registration waits deliberately do not poll. Afterwards, exercise the real
 * dispatch boundary and require its bounded queue to become empty. */
static int drain_service(h2_gizclaw_service_t *service) {
  int rc = H2_PAL_ERR_TIMEOUT;
  for (unsigned attempt = 0u; attempt < 32u; ++attempt) {
    size_t dispatched = SIZE_MAX;
    rc = h2_gizclaw_service_poll(service, 8u, &dispatched);
    h2_gizclaw_e2e_evidence("h2_gizclaw_service_poll", "service", rc);
    if (rc != H2_PAL_OK)
      break;
    if (dispatched > 8u) {
      rc = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    if (dispatched == 0u)
      break;
    rc = H2_PAL_ERR_TIMEOUT;
  }
  return rc;
}

/* A connected Service has completed at least one calibration attempt. Host
 * clocks may be unsettable; report that outcome without claiming calibration. */
static int check_time_sync(h2_gizclaw_service_t *service) {
  h2_gizclaw_time_sync_status_t status = {0};
  int rc = h2_gizclaw_service_get_time_sync_status(service, &status);
  h2_gizclaw_e2e_evidence("h2_gizclaw_service_get_time_sync_status", "service", rc);
  if (rc == H2_PAL_OK &&
      (status.attempts == 0u || status.state < H2_GIZCLAW_TIME_SYNC_RUNNING ||
       status.state > H2_GIZCLAW_TIME_SYNC_SUCCEEDED ||
       (status.state == H2_GIZCLAW_TIME_SYNC_SUCCEEDED &&
        status.last_result != H2_PAL_OK) ||
       (status.state == H2_GIZCLAW_TIME_SYNC_RETRY &&
        status.last_result == H2_PAL_OK)))
    rc = H2_PAL_ERR_INVALID_STATE;
  printf("H2_GIZCLAW_E2E stage=time-sync-state state=%u attempts=%u "
         "last_rc=%d calibrated=%u result=%s rc=%d\n",
         (unsigned)status.state, status.attempts, status.last_result,
         rc == H2_PAL_OK && status.state == H2_GIZCLAW_TIME_SYNC_SUCCEEDED,
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_service_get_time_sync_status",
                         "service_get_time_sync_status-assert", rc);
  return rc;
}

int h2_gizclaw_e2e_run_service(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->allocator == NULL || fixture->time == NULL ||
      fixture->registration_token == NULL ||
      fixture->registration_token[0] == '\0' ||
      fixture->runtime_profile_name[0] == '\0' ||
      memchr(fixture->runtime_profile_name, '\0',
             sizeof(fixture->runtime_profile_name)) == NULL ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, REQUEST_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_req_t *request = NULL;
  int rc =
      h2_gizclaw_req_create_register(service, 1u, fixture->registration_token,
                                     REQUEST_TIMEOUT_MS, &request);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_create_register", "service", rc);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_do", "service", rc);
  }
  if (rc == H2_PAL_OK) {
    const int duplicate_rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    rc = expect_result("h2_gizclaw_req_do", duplicate_rc,
                       H2_PAL_ERR_INVALID_STATE);
  }
  for (unsigned i = 0u; rc == H2_PAL_OK && i < 2u; ++i) {
    /* No service_poll: terminal publication and repeated waits must not depend
     * on app dispatch. The second wait observes an already terminal request. */
    const uint32_t timeout = i == 0u ? REQUEST_TIMEOUT_MS : 1u;
    rc = h2_gizclaw_e2e_fixture_has_time(fixture, timeout)
             ? h2_gizclaw_req_wait(request, timeout)
             : H2_PAL_ERR_TIMEOUT;
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait", "service-no-poll", rc);
  }
  if (rc == H2_PAL_OK) {
    h2_gizclaw_registration_result_t registration = {0};
    rc = h2_gizclaw_resp_parse_register(request, &registration);
    if (rc == H2_PAL_OK &&
        (memchr(registration.runtime_profile_name, '\0',
                sizeof(registration.runtime_profile_name)) == NULL ||
         strcmp(registration.runtime_profile_name,
                fixture->runtime_profile_name) != 0))
      rc = H2_PAL_ERR_INVALID_STATE;
    h2_gizclaw_e2e_evidence("h2_gizclaw_resp_parse_register", "service", rc);
  }
  if (rc != H2_PAL_OK && request != NULL)
    (void)h2_gizclaw_req_cancel(request);
  h2_gizclaw_req_release(request);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_release", "service", H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_do", "req_do-assert", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait", "req_wait-assert", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_req_release", "req_release-assert", rc);
  if (rc == H2_PAL_OK)
    rc = canceled_before_do(fixture, service);
  if (rc == H2_PAL_OK) {
    rc = drain_service(service);
    h2_gizclaw_e2e_evidence("h2_gizclaw_service_poll", "service_poll-assert", rc);
  }
  if (rc == H2_PAL_OK)
    rc = check_time_sync(service);
  return rc;
}
