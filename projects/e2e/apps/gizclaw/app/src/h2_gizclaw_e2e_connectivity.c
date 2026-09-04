#include "h2_gizclaw_e2e_rpc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SPEED_BYTES (2u * 1024u * 1024u)
#define SPEED_REPETITIONS 3u
#define REQUEST_TIMEOUT_MS 30000u

enum method { PING, SPEED, REGISTER, DELETE };
static const char *const creates[] = {
    "h2_gizclaw_req_create_ping", "h2_gizclaw_req_create_speedtest",
    "h2_gizclaw_req_create_register", "h2_gizclaw_req_create_peer_delete"};
static const char *const parses[] = {
    "h2_gizclaw_resp_parse_ping", "h2_gizclaw_resp_parse_speedtest",
    "h2_gizclaw_resp_parse_register", "h2_gizclaw_resp_parse_peer_delete"};
static const char *const rpcs[] = {
    "h2_gizclaw_rpc_ping", "h2_gizclaw_rpc_speedtest",
    "h2_gizclaw_rpc_register", "h2_gizclaw_rpc_peer_delete"};
static const char *const proofs[] = {"ping-assert", "speedtest-assert",
                                     "register-assert", "peer_delete-assert"};

static int evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static h2_pal_result_t speed_input(void *user, uint8_t *buffer,
                                   size_t capacity, size_t *out_read) {
  h2_gizclaw_e2e_speed_hooks_t *hooks = user;
  if (hooks == NULL || buffer == NULL || out_read == NULL ||
      hooks->bytes > hooks->expected_bytes)
    return H2_PAL_ERR_INVALID_ARG;
  size_t count = hooks->expected_bytes - hooks->bytes;
  if (count > capacity)
    count = capacity;
  hooks->bytes += count;
  ++hooks->chunks;
  *out_read = count;
  return H2_PAL_OK;
}

static int drain_speed_hooks(h2_gizclaw_e2e_fixture_t *fixture,
                             h2_gizclaw_service_t *service,
                             h2_gizclaw_e2e_speed_hooks_t *hooks) {
  uint64_t started = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &started);
  while (rc == H2_PAL_OK) {
    size_t dispatched = 0u;
    rc = h2_gizclaw_service_poll(service, 8u, &dispatched);
    if (rc == H2_PAL_OK)
      rc = hooks->error;
    if (rc != H2_PAL_OK)
      break;
    rc = h2_gizclaw_req_wait(hooks->request, 1u);
    if (rc != H2_PAL_ERR_TIMEOUT)
      break;
    rc = H2_PAL_OK;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(fixture->time, &now);
    if (rc == H2_PAL_OK && now < started)
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK &&
        (now - started >= REQUEST_TIMEOUT_MS || now >= fixture->deadline_ms))
      rc = H2_PAL_ERR_TIMEOUT;
    if (rc == H2_PAL_OK && dispatched == 0u)
      rc = h2_pal_time_sleep_ms(fixture->time, 1u);
  }
  return rc;
}

static const char *platform_name(void) {
#if defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#else
  return "mcu";
#endif
}

static int call(h2_gizclaw_e2e_fixture_t *fixture, unsigned api,
                enum method method, uint64_t identity, size_t upload,
                size_t download, h2_gizclaw_speedtest_result_t *speed) {
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, REQUEST_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  /* Both speed APIs use OWNER. The second isolated actor is only needed to
   * exercise the second delete API after all measurements have finished. */
  h2_gizclaw_e2e_actor_t *actor =
      &fixture->actors[method == DELETE && api == 1u ? H2_GIZCLAW_E2E_FRIEND
                                                     : H2_GIZCLAW_E2E_OWNER];
  h2_gizclaw_service_t *service = actor->service;
  h2_gizclaw_ping_result_t ping = {0};
  h2_gizclaw_registration_result_t registration = {0};
  int rc = H2_PAL_ERR_INVALID_STATE;
  if (api == 0u) {
    h2_gizclaw_req_t *request = NULL;
    h2_gizclaw_e2e_speed_hooks_t *hooks = NULL;
    if (method == SPEED) {
      if (identity < 11u || identity > 16u)
        return H2_PAL_ERR_INVALID_ARG;
      hooks = &fixture->speed_hooks[identity - 11u];
      if (hooks->request != NULL)
        return H2_PAL_ERR_INVALID_STATE;
      hooks->expected_bytes = upload != 0u ? upload : download;
    }
    switch (method) {
    case PING:
      rc = h2_gizclaw_req_create_ping(service, identity, REQUEST_TIMEOUT_MS,
                                      &request);
      break;
    case SPEED:
      rc = h2_gizclaw_req_create_speedtest(service, identity, upload, download,
                                           REQUEST_TIMEOUT_MS, &request);
      break;
    case REGISTER:
      rc = h2_gizclaw_req_create_register(service, identity,
                                          fixture->registration_token,
                                          REQUEST_TIMEOUT_MS, &request);
      break;
    case DELETE:
      rc = h2_gizclaw_req_create_peer_delete(service, identity,
                                             REQUEST_TIMEOUT_MS, &request);
      break;
    }
    evidence(creates[method], "connectivity-req", rc);
    if (hooks != NULL)
      hooks->request = request;
    if (rc == H2_PAL_OK)
      rc = evidence("h2_gizclaw_req_do", "connectivity-req",
                    h2_gizclaw_req_do(
                        request, hooks, upload != 0u ? speed_input : NULL,
                        NULL, NULL));
    if (rc == H2_PAL_OK && hooks != NULL) {
      rc = drain_speed_hooks(fixture, service, hooks);
      evidence("h2_gizclaw_service_poll", "connectivity-chunks", rc);
      evidence("h2_gizclaw_service_poll", "speedtest-chunks-assert", rc);
      printf("H2_GIZCLAW_E2E stage=speedtest-hooks request=%" PRIu64
             " direction=%s bytes=%zu chunks=%zu "
             "result=%s rc=%d\n",
             identity, upload ? "upload" : "download",
             upload != 0u ? hooks->bytes : download,
             upload != 0u ? hooks->chunks : 0u,
             rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
    }
    if (rc == H2_PAL_OK)
      rc = evidence("h2_gizclaw_req_wait", "connectivity-req",
                    h2_gizclaw_req_wait(request, REQUEST_TIMEOUT_MS));
    if (rc == H2_PAL_OK) {
      switch (method) {
      case PING:
        rc = h2_gizclaw_resp_parse_ping(request, &ping);
        break;
      case SPEED:
        rc = h2_gizclaw_resp_parse_speedtest(request, speed);
        break;
      case REGISTER:
        rc = h2_gizclaw_resp_parse_register(request, &registration);
        break;
      case DELETE:
        rc = h2_gizclaw_resp_parse_peer_delete(request);
        break;
      }
      evidence(parses[method], "connectivity-req", rc);
    }
    if (rc != H2_PAL_OK && request != NULL)
      (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  } else {
    switch (method) {
    case PING:
      rc = h2_gizclaw_rpc_ping(service, REQUEST_TIMEOUT_MS, &ping);
      break;
    case SPEED:
      rc = h2_gizclaw_rpc_speedtest(service, upload, download,
                                    REQUEST_TIMEOUT_MS, speed);
      break;
    case REGISTER:
      rc = h2_gizclaw_rpc_register(service, fixture->registration_token,
                                   REQUEST_TIMEOUT_MS, &registration);
      break;
    case DELETE:
      rc = h2_gizclaw_rpc_peer_delete(service, REQUEST_TIMEOUT_MS);
      break;
    }
    evidence(rpcs[method], "connectivity-rpc", rc);
  }
  if (rc == H2_PAL_OK && method == PING && ping.server_time_ms <= 0)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && method == REGISTER &&
      (memchr(registration.runtime_profile_name, '\0',
              sizeof(registration.runtime_profile_name)) == NULL ||
       strcmp(registration.runtime_profile_name,
              fixture->runtime_profile_name) != 0))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && method == DELETE) {
    /* Parser/RPC success acknowledges deletion. Timeouts and invalid responses
     * retain the obligation; closing a transport is never deletion evidence. */
    actor->peer_delete_requested = true;
    actor->peer_delete_required = false;
    actor->registered = false;
  }
  if (method != SPEED)
    evidence(api == 0u ? parses[method] : rpcs[method], proofs[method], rc);
  return rc;
}

int h2_gizclaw_e2e_run_connectivity(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->time == NULL ||
      fixture->registration_token == NULL ||
      fixture->registration_token[0] == '\0' ||
      fixture->runtime_profile_name[0] == '\0' ||
      memchr(fixture->runtime_profile_name, '\0',
             sizeof(fixture->runtime_profile_name)) == NULL ||
      memchr(fixture->endpoint, '\0', sizeof(fixture->endpoint)) == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < 2u; ++i)
    if (fixture->actors[i].service == NULL || !fixture->actors[i].registered ||
        !fixture->actors[i].peer_delete_required)
      return H2_PAL_ERR_INVALID_STATE;
  if (fixture->actors[0].service == fixture->actors[1].service)
    return H2_PAL_ERR_INVALID_STATE;
  for (unsigned api = 0u; api < 2u; ++api) {
    int rc = call(fixture, api, REGISTER, 1u + api, 0u, 0u, NULL);
    if (rc == H2_PAL_OK)
      rc = call(fixture, api, PING, 3u + api, 0u, 0u, NULL);
    if (rc != H2_PAL_OK)
      return rc;
  }
  int result = H2_PAL_OK;
  for (unsigned api = 0u; api < 2u; ++api) {
    for (unsigned direction = 0u; direction < 2u; ++direction) {
      for (unsigned attempt = 1u; attempt <= SPEED_REPETITIONS; ++attempt) {
        const size_t upload = direction == 0u ? SPEED_BYTES : 0u;
        const size_t download = direction == 1u ? SPEED_BYTES : 0u;
        h2_gizclaw_speedtest_result_t speed = {0};
        uint64_t started = 0u, completed = 0u;
        int rc = h2_pal_time_get_monotonic_ms(fixture->time, &started);
        if (rc == H2_PAL_OK)
          rc = call(fixture, api, SPEED,
                    10u + direction * SPEED_REPETITIONS + attempt, upload,
                    download, &speed);
        const int clock_rc =
            h2_pal_time_get_monotonic_ms(fixture->time, &completed);
        if (rc == H2_PAL_OK)
          rc = clock_rc;
        const uint64_t bytes =
            direction == 0u ? speed.upload_bytes : speed.download_bytes;
        const uint64_t elapsed =
            direction == 0u ? speed.upload_elapsed_ms : speed.elapsed_ms;
        const uint64_t bps = direction == 0u ? speed.upload_bits_per_second
                                             : speed.download_bits_per_second;
        if (rc == H2_PAL_OK &&
            (speed.upload_bytes != upload || speed.download_bytes != download ||
             elapsed == 0u || bps == 0u || bps != bytes * 8000u / elapsed ||
             completed < started))
          rc = H2_PAL_ERR_INVALID_STATE;
        evidence(api == 0u ? parses[SPEED] : rpcs[SPEED], proofs[SPEED], rc);
        printf(
            "H2_GIZCLAW_E2E stage=speedtest platform=%s endpoint=%s "
            "runtime_profile=%s api=%s direction=%s attempt=%u repetitions=%u "
            "expected_bytes=%u bytes=%" PRIu64 " transfer_ms=%" PRIu64
            " request_ms=%" PRIu64 " bps=%" PRIu64
            " Mbps=%.3f integrity=%s result=%s rc=%d\n",
            platform_name(), fixture->endpoint, fixture->runtime_profile_name,
            api == 0u ? "req" : "rpc", direction == 0u ? "upload" : "download",
            attempt, SPEED_REPETITIONS, SPEED_BYTES, bytes, elapsed,
            completed >= started ? completed - started : 0u, bps,
            (double)bps / 1000000.0,
            rc != H2_PAL_OK   ? "not-verified"
            : direction == 0u ? "length-ack-only"
                              : "pattern-verified",
            rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
        if (result == H2_PAL_OK)
          result = rc;
      }
    }
  }
  /* A later poll can expose a duplicate callback for an earlier measurement.
   * Do not lose that violation merely because its original call returned. */
  for (size_t i = 0u; result == H2_PAL_OK && i < SPEED_REPETITIONS * 2u; ++i)
    result = fixture->speed_hooks[i].error;
  for (unsigned api = 0u; result == H2_PAL_OK && api < 2u; ++api)
    result = call(fixture, api, DELETE, 30u + api, 0u, 0u, NULL);
  return result;
}
