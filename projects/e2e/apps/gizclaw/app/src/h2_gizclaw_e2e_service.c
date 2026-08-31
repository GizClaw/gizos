#include "h2_gizclaw_e2e_service.h"

#include "h2/pal/os/h2_pal_time.h"
#include "h2_gizclaw_service.h"

#include <stdatomic.h>
#include <string.h>

#define H2_GIZCLAW_E2E_SERVICE_OPERATION_COUNT 1u
#define H2_GIZCLAW_E2E_SERVICE_POLL_MS 10u

typedef struct service_case_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_service_t *service;
  h2_gizclaw_registration_request_t *registration_request;
  atomic_size_t completion_count;
  atomic_int result;
  atomic_uint terminal_count;
} service_case_state_t;

static void keep_first_failure(int candidate, int *result) {
  if (*result == H2_PAL_OK && candidate != H2_PAL_OK)
    *result = candidate;
}

static void keep_first_atomic_failure(int candidate, atomic_int *result) {
  int expected = H2_PAL_OK;
  if (candidate != H2_PAL_OK) {
    (void)atomic_compare_exchange_strong_explicit(
        result, &expected, candidate, memory_order_acq_rel,
        memory_order_acquire);
  }
}

static void registration_completion(
    void *user, h2_gizclaw_registration_request_t *request,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_registration_result_t *registration) {
  service_case_state_t *state = user;
  int callback_rc = H2_PAL_OK;
  if (state == NULL || request == NULL || result == NULL)
    return;
  const size_t completion_count =
      atomic_load_explicit(&state->completion_count, memory_order_relaxed);
  if (request != state->registration_request || completion_count != 0u ||
      result->identity != 1u ||
      result->terminal_kind != H2_GIZCLAW_OPERATION_FINISHED ||
      result->result != H2_PAL_OK || registration == NULL ||
      strcmp(registration->runtime_profile_name,
             state->fixture->runtime_profile_name) != 0) {
    callback_rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_service_register_async", "service",
                          callback_rc);
  state->registration_request = NULL;
  keep_first_atomic_failure(callback_rc, &state->result);
  atomic_store_explicit(&state->completion_count, completion_count + 1u,
                        memory_order_release);
  h2_gizclaw_registration_request_release(request);
}

static void service_terminal(void *user, h2_pal_result_t result) {
  service_case_state_t *state = user;
  if (state == NULL)
    return;
  atomic_fetch_add_explicit(&state->terminal_count, 1u, memory_order_release);
  keep_first_atomic_failure(
      result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result,
      &state->result);
}

static int dispatch_until_complete(service_case_state_t *state,
                                   size_t expected_completions) {
  while (atomic_load_explicit(&state->completion_count,
                              memory_order_acquire) < expected_completions) {
    int rc = H2_PAL_OK;
    if (state->fixture->config->should_stop != NULL &&
        state->fixture->config->should_stop(
            state->fixture->config->should_stop_user)) {
      return H2_PAL_ERR_CLOSED;
    }
    if (!h2_gizclaw_e2e_fixture_has_time(state->fixture,
                                         H2_GIZCLAW_E2E_SERVICE_POLL_MS)) {
      return H2_PAL_ERR_TIMEOUT;
    }
    rc = h2_pal_time_sleep_ms(state->fixture->time,
                              H2_GIZCLAW_E2E_SERVICE_POLL_MS);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static int cleanup_service_case(service_case_state_t *state,
                                size_t accepted_operations) {
  if (state->registration_request != NULL)
    (void)h2_gizclaw_registration_request_cancel(
        state->registration_request);
  int rc = h2_gizclaw_service_stop(state->service);
  if (atomic_load_explicit(&state->completion_count, memory_order_acquire) !=
      accepted_operations)
    keep_first_failure(H2_PAL_ERR_INVALID_STATE, &rc);
  const int deinit_rc = h2_gizclaw_service_deinit(state->service);
  if (deinit_rc == H2_PAL_OK)
    state->service = NULL;
  keep_first_failure(deinit_rc, &rc);
  return rc;
}

int h2_gizclaw_e2e_run_service(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->runtime == NULL ||
      fixture->runtime->task == NULL || fixture->runtime->queue == NULL ||
      fixture->runtime->sync == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_config_t client_config;
  memset(&client_config, 0, sizeof(client_config));
  int rc = h2_gizclaw_e2e_fixture_transfer_actor_to_service(
      fixture, H2_GIZCLAW_E2E_OWNER, &client_config);
  if (rc != H2_PAL_OK)
    return rc;

  service_case_state_t state;
  memset(&state, 0, sizeof(state));
  state.fixture = fixture;
  atomic_init(&state.completion_count, 0u);
  atomic_init(&state.result, H2_PAL_OK);
  atomic_init(&state.terminal_count, 0u);
  const h2_gizclaw_service_config_t service_config = {
      .client_config = &client_config,
      .task = fixture->runtime->task,
      .queue = fixture->runtime->queue,
      .sync = fixture->runtime->sync,
      .net_task_options = {.min_stack_size = 32768u},
      .resp_dispatch_task_options = {.min_stack_size = 32768u},
      .operation_capacity = H2_GIZCLAW_E2E_SERVICE_OPERATION_COUNT,
      .client_poll_timeout_ms = (int)H2_GIZCLAW_E2E_SERVICE_POLL_MS,
      .terminal = service_terminal,
      .terminal_user = &state,
  };
  rc = h2_gizclaw_service_init(&service_config, &state.service);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_gizclaw_service_start(state.service);
  size_t accepted_operations = 0u;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_register_async(
        state.service, 1u, fixture->registration_token, 30000u,
        registration_completion, &state, &state.registration_request);
    if (rc == H2_PAL_OK)
      accepted_operations++;
  }
  if (rc == H2_PAL_OK)
    rc = dispatch_until_complete(&state, accepted_operations);
  if (rc == H2_PAL_OK &&
      (atomic_load_explicit(&state.completion_count, memory_order_acquire) !=
           H2_GIZCLAW_E2E_SERVICE_OPERATION_COUNT ||
       atomic_load_explicit(&state.terminal_count, memory_order_acquire) != 0u ||
       atomic_load_explicit(&state.result, memory_order_acquire) != H2_PAL_OK)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  const int cleanup_rc = cleanup_service_case(&state, accepted_operations);
  keep_first_failure(cleanup_rc, &rc);
  if (state.registration_request != NULL)
    keep_first_failure(H2_PAL_ERR_INVALID_STATE, &rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_service_deinit", "service-cleanup",
                          cleanup_rc);
  return rc;
}
