#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_report.h"

#include <inttypes.h>
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
  if (fixture == NULL || fixture->actors[H2_GIZCLAW_E2E_OWNER].client == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_rpc_request_t *requests[H2_GIZCLAW_E2E_CONCURRENT_REQUESTS] = {0};
  bool terminal[H2_GIZCLAW_E2E_CONCURRENT_REQUESTS] = {false};
  size_t started_requests = 0u;
  size_t completed_requests = 0u;
  int requests_result = H2_PAL_OK;

  h2_gizclaw_e2e_fixture_reset_rpc_channel_observation();
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_CONCURRENT_REQUESTS; ++index) {
    const int rc = h2_gizclaw_client_rpc_request_start(
        client, H2_GIZCLAW_RPC_ALL_PING, (h2_gizclaw_rpc_bytes_t){0}, 30000u,
        &requests[index]);
    if (rc != H2_PAL_OK) {
      requests_result = rc;
      break;
    }
    started_requests++;
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_rpc_request_start", "concurrency",
                          started_requests == H2_GIZCLAW_E2E_CONCURRENT_REQUESTS
                              ? H2_PAL_OK
                              : requests_result);

  while (requests_result == H2_PAL_OK &&
         completed_requests < started_requests &&
         h2_gizclaw_e2e_fixture_has_time(fixture, 50u)) {
    const int poll_rc = h2_gizclaw_client_poll(client, 50);
    if (poll_rc != H2_PAL_OK && poll_rc != H2_PAL_ERR_TIMEOUT &&
        poll_rc != H2_PAL_ERR_WOULD_BLOCK) {
      requests_result = poll_rc;
      break;
    }
    for (size_t index = 0u; index < started_requests; ++index) {
      if (terminal[index])
        continue;
      h2_gizclaw_rpc_response_t response = {0};
      int rc = h2_gizclaw_rpc_request_result(requests[index], &response);
      if (rc == H2_PAL_ERR_WOULD_BLOCK)
        continue;
      terminal[index] = true;
      completed_requests++;
      if (rc == H2_PAL_OK && response.has_error)
        rc = H2_PAL_ERR_IO;
      h2_gizclaw_rpc_response_deinit(client, &response);
      if (requests_result == H2_PAL_OK && rc != H2_PAL_OK)
        requests_result = rc;
    }
  }
  if (requests_result == H2_PAL_OK &&
      completed_requests != H2_GIZCLAW_E2E_CONCURRENT_REQUESTS) {
    requests_result = H2_PAL_ERR_TIMEOUT;
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_rpc_request_result", "concurrency",
                          requests_result == H2_PAL_OK &&
                                  completed_requests ==
                                      H2_GIZCLAW_E2E_CONCURRENT_REQUESTS
                              ? H2_PAL_OK
                              : requests_result);
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_CONCURRENT_REQUESTS; ++index) {
    h2_gizclaw_rpc_request_cancel(requests[index]);
    h2_gizclaw_rpc_request_destroy(requests[index]);
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_rpc_request_cancel", "concurrency",
                          started_requests == H2_GIZCLAW_E2E_CONCURRENT_REQUESTS
                              ? H2_PAL_OK
                              : requests_result);
  h2_gizclaw_e2e_evidence("h2_gizclaw_rpc_request_destroy", "concurrency",
                          started_requests == H2_GIZCLAW_E2E_CONCURRENT_REQUESTS
                              ? H2_PAL_OK
                              : requests_result);

  size_t max_open_channels = 0u;
  size_t unique_stream_ids = 0u;
  size_t open_channels = 0u;
  const int observation_result = h2_gizclaw_e2e_fixture_rpc_channel_observation(
      &max_open_channels, &unique_stream_ids, &open_channels);

  h2_gizclaw_ping_result_t recovery_ping = {0};
  const int recovery_result =
      h2_gizclaw_client_ping_measure(client, &recovery_ping);
  const int result = h2_gizclaw_e2e_concurrency_classify(
      requests_result, recovery_result, observation_result, started_requests,
      completed_requests, max_open_channels, unique_stream_ids, open_channels);

  printf("H2_GIZCLAW_E2E stage=concurrency clients=1 "
         "requested_requests=%u started_requests=%zu completed_requests=%zu "
         "max_open_channels=%zu "
         "unique_stream_ids=%zu open_channels=%zu observation_rc=%d "
         "requests_rc=%d recovery_rc=%d recovery_rtt_ms=%" PRIu64
         " result=%s rc=%d\n",
         H2_GIZCLAW_E2E_CONCURRENT_REQUESTS, started_requests,
         completed_requests, max_open_channels, unique_stream_ids,
         open_channels, observation_result, requests_result, recovery_result,
         recovery_ping.round_trip_ms, result == H2_PAL_OK ? "PASS" : "FAIL",
         result);
  return result;
}
