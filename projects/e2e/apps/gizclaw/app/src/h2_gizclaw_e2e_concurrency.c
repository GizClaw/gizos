#include "h2_gizclaw_e2e_concurrency.h"

#include <stdio.h>

enum { H2_GIZCLAW_E2E_CONCURRENT_REQUESTS = 3 };

int h2_gizclaw_e2e_concurrency_classify(
    int requests_result, int recovery_result, int observation_result,
    size_t started_requests, size_t completed_requests,
    size_t max_open_channels, size_t unique_stream_ids, size_t open_channels) {
  if (requests_result != H2_PAL_OK)
    return requests_result;
  if (recovery_result != H2_PAL_OK)
    return recovery_result;
  if (observation_result != H2_PAL_OK)
    return observation_result;
  if (started_requests != H2_GIZCLAW_E2E_CONCURRENT_REQUESTS ||
      completed_requests != H2_GIZCLAW_E2E_CONCURRENT_REQUESTS ||
      max_open_channels == 0u ||
      max_open_channels > H2_GIZCLAW_E2E_CONCURRENT_REQUESTS ||
      unique_stream_ids != H2_GIZCLAW_E2E_CONCURRENT_REQUESTS ||
      open_channels != 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_run_concurrency(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  h2_gizclaw_req_t *requests[H2_GIZCLAW_E2E_CONCURRENT_REQUESTS] = {0};
  size_t started = 0u, completed = 0u;
  int requests_result = H2_PAL_OK;
  h2_gizclaw_e2e_fixture_reset_rpc_channel_observation();
  /* All requests are admitted before waiting for any of them. */
  for (size_t i = 0u; i < H2_GIZCLAW_E2E_CONCURRENT_REQUESTS; ++i) {
    int rc = h2_gizclaw_req_create_ping(service, i + 1u, 30000u, &requests[i]);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_req_do(requests[i], NULL, NULL, NULL, NULL);
    if (rc != H2_PAL_OK) {
      requests_result = rc;
      break;
    }
    ++started;
  }
  for (size_t i = 0u; i < started; ++i) {
    int rc = h2_gizclaw_e2e_fixture_has_time(fixture, 1u)
                 ? h2_gizclaw_req_wait(requests[i], 30000u)
                 : H2_PAL_ERR_TIMEOUT;
    h2_gizclaw_ping_result_t response = {0};
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_resp_parse_ping(requests[i], &response);
    h2_gizclaw_e2e_evidence("h2_gizclaw_resp_parse_ping", "concurrency-no-poll",
                            rc);
    if (rc == H2_PAL_OK)
      ++completed;
    else if (requests_result == H2_PAL_OK)
      requests_result = rc;
  }
  for (size_t i = 0u; i < H2_GIZCLAW_E2E_CONCURRENT_REQUESTS; ++i) {
    if (requests[i] == NULL)
      continue;
    if (requests_result != H2_PAL_OK && i < started)
      (void)h2_gizclaw_req_cancel(requests[i]);
    h2_gizclaw_req_release(requests[i]);
  }

  size_t maximum = 0u, unique = 0u, open = 0u;
  int observation_result;
  /* Channel close events may follow request settlement. The net task continues
   * consuming them without an application client/protocol poll call. */
  do {
    observation_result = h2_gizclaw_e2e_fixture_rpc_channel_observation(
        &maximum, &unique, &open);
    if (observation_result != H2_PAL_OK || open == 0u)
      break;
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, 1u)) {
      observation_result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    observation_result = h2_pal_time_sleep_ms(fixture->time, 1u);
  } while (observation_result == H2_PAL_OK);
  h2_gizclaw_ping_result_t recovery = {0};
  const int recovery_result =
      h2_gizclaw_e2e_fixture_has_time(fixture, 1u)
          ? h2_gizclaw_rpc_ping(service, 30000u, &recovery)
          : H2_PAL_ERR_TIMEOUT;
  const int result = h2_gizclaw_e2e_concurrency_classify(
      requests_result, recovery_result, observation_result, started, completed,
      maximum, unique, open);
  printf("H2_GIZCLAW_E2E stage=concurrency services=1 requested_requests=%u "
         "started_requests=%zu completed_requests=%zu max_open_channels=%zu "
         "unique_stream_ids=%zu open_channels=%zu observation_rc=%d "
         "requests_rc=%d recovery_rc=%d result=%s rc=%d\n",
         H2_GIZCLAW_E2E_CONCURRENT_REQUESTS, started, completed, maximum,
         unique, open, observation_result, requests_result, recovery_result,
         result == H2_PAL_OK ? "PASS" : "FAIL", result);
  return result;
}
