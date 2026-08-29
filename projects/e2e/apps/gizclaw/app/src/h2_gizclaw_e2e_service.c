#include "h2_gizclaw_e2e_service.h"

#include "h2/pal/os/h2_pal_time.h"
#include "h2_gizclaw_service.h"

#include <stdatomic.h>
#include <string.h>

#define H2_GIZCLAW_E2E_SERVICE_OPERATION_COUNT 2u
#define H2_GIZCLAW_E2E_SERVICE_DISPATCH_BOUND 2u
#define H2_GIZCLAW_E2E_SERVICE_POLL_MS 10u

typedef struct service_case_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *request_operation;
  h2_gizclaw_operation_t *canceled_operation;
  size_t completion_count;
  int result;
  unsigned progress_count;
  unsigned terminal_count;
  atomic_bool canceled_operation_ran;
} service_case_state_t;

static void keep_first_failure(int candidate, int *result) {
  if (*result == H2_PAL_OK && candidate != H2_PAL_OK)
    *result = candidate;
}

static h2_pal_result_t service_progress(void *user) {
  service_case_state_t *state = user;
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  state->progress_count++;
  h2_gizclaw_e2e_evidence("h2_gizclaw_operation_dispatch_call",
                          "service-progress", H2_PAL_OK);
  return H2_PAL_OK;
}

static h2_pal_result_t
run_registered_ping(void *user, h2_gizclaw_client_t *client,
                    const h2_gizclaw_cancel_token_t *cancel_token) {
  service_case_state_t *state = user;
  if (state == NULL || client == NULL || cancel_token == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_registration_result_t registration;
  memset(&registration, 0, sizeof(registration));
  int rc = h2_gizclaw_client_register(
      client, state->fixture->registration_token, &registration);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_register", "service", rc);
  if (rc == H2_PAL_OK && strcmp(registration.runtime_profile_name,
                                state->fixture->runtime_profile_name) != 0) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_operation_dispatch_call(cancel_token, service_progress,
                                            state);
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_operation_dispatch_call", "service", rc);
  h2_gizclaw_ping_result_t ping;
  memset(&ping, 0, sizeof(ping));
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_client_ping_measure(client, &ping);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_ping_measure", "service", rc);
  return rc;
}

static h2_pal_result_t
run_canceled_operation(void *user, h2_gizclaw_client_t *client,
                       const h2_gizclaw_cancel_token_t *cancel_token) {
  service_case_state_t *state = user;
  (void)client;
  (void)cancel_token;
  atomic_store_explicit(&state->canceled_operation_ran, true,
                        memory_order_release);
  return H2_PAL_ERR_INVALID_STATE;
}

static void service_completion(void *user, h2_gizclaw_operation_t *operation,
                               const h2_gizclaw_operation_result_t *result) {
  service_case_state_t *state = user;
  int callback_rc = H2_PAL_OK;
  if (state == NULL || operation == NULL || result == NULL)
    return;
  if (operation == state->request_operation) {
    if (state->completion_count != 0u || result->identity != 1u ||
        result->terminal_kind != H2_GIZCLAW_OPERATION_FINISHED ||
        result->result != H2_PAL_OK) {
      callback_rc = H2_PAL_ERR_INVALID_STATE;
    }
    state->request_operation = NULL;
  } else if (operation == state->canceled_operation) {
    if (state->completion_count != 1u || result->identity != 2u ||
        result->terminal_kind != H2_GIZCLAW_OPERATION_CANCELED ||
        result->result != H2_PAL_ERR_CLOSED ||
        atomic_load_explicit(&state->canceled_operation_ran,
                             memory_order_acquire)) {
      callback_rc = H2_PAL_ERR_INVALID_STATE;
    }
    state->canceled_operation = NULL;
  } else {
    callback_rc = H2_PAL_ERR_INVALID_STATE;
  }
  keep_first_failure(callback_rc, &state->result);
  state->completion_count++;
  h2_gizclaw_operation_release(operation);
}

static void service_terminal(void *user, h2_pal_result_t result) {
  service_case_state_t *state = user;
  if (state == NULL)
    return;
  state->terminal_count++;
  keep_first_failure(result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result,
                     &state->result);
}

static int dispatch_until_complete(service_case_state_t *state,
                                   size_t expected_completions) {
  while (state->completion_count < expected_completions) {
    size_t dispatched = 0u;
    int rc = h2_gizclaw_service_dispatch(
        state->service, H2_GIZCLAW_E2E_SERVICE_DISPATCH_BOUND, &dispatched);
    if (rc != H2_PAL_OK)
      return rc;
    if (state->fixture->config->should_stop != NULL &&
        state->fixture->config->should_stop(
            state->fixture->config->should_stop_user)) {
      return H2_PAL_ERR_CLOSED;
    }
    if (!h2_gizclaw_e2e_fixture_has_time(state->fixture,
                                         H2_GIZCLAW_E2E_SERVICE_POLL_MS)) {
      return H2_PAL_ERR_TIMEOUT;
    }
    if (dispatched == 0u) {
      rc = h2_pal_time_sleep_ms(state->fixture->time,
                                H2_GIZCLAW_E2E_SERVICE_POLL_MS);
      if (rc != H2_PAL_OK)
        return rc;
    }
  }
  return H2_PAL_OK;
}

static int cleanup_service_case(service_case_state_t *state,
                                size_t accepted_operations) {
  if (state->request_operation != NULL)
    (void)h2_gizclaw_operation_cancel(state->request_operation);
  if (state->canceled_operation != NULL)
    (void)h2_gizclaw_operation_cancel(state->canceled_operation);
  int rc = h2_gizclaw_service_stop(state->service);
  while (state->completion_count < accepted_operations) {
    size_t dispatched = 0u;
    const int dispatch_rc = h2_gizclaw_service_dispatch(
        state->service, H2_GIZCLAW_E2E_SERVICE_DISPATCH_BOUND, &dispatched);
    if (dispatch_rc != H2_PAL_OK || dispatched == 0u) {
      keep_first_failure(dispatch_rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE
                                                  : dispatch_rc,
                         &rc);
      break;
    }
  }
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
  state.result = H2_PAL_OK;
  atomic_init(&state.canceled_operation_ran, false);
  const h2_gizclaw_service_config_t service_config = {
      .client_config = &client_config,
      .task = fixture->runtime->task,
      .queue = fixture->runtime->queue,
      .sync = fixture->runtime->sync,
      .task_options = {.min_stack_size = 32768u},
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
    rc = h2_gizclaw_service_submit(state.service, 1u, run_registered_ping,
                                   service_completion, &state,
                                   &state.request_operation);
    if (rc == H2_PAL_OK)
      accepted_operations++;
  }
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_submit(state.service, 2u, run_canceled_operation,
                                   service_completion, &state,
                                   &state.canceled_operation);
    if (rc == H2_PAL_OK)
      accepted_operations++;
  }
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_operation_cancel(state.canceled_operation);
  if (rc == H2_PAL_OK)
    rc = dispatch_until_complete(&state, accepted_operations);
  if (rc == H2_PAL_OK &&
      (state.progress_count != 1u ||
       state.completion_count != H2_GIZCLAW_E2E_SERVICE_OPERATION_COUNT ||
       state.terminal_count != 0u || state.result != H2_PAL_OK)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  const int cleanup_rc = cleanup_service_case(&state, accepted_operations);
  keep_first_failure(cleanup_rc, &rc);
  if (state.request_operation != NULL || state.canceled_operation != NULL)
    keep_first_failure(H2_PAL_ERR_INVALID_STATE, &rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_service_deinit", "service-cleanup",
                          cleanup_rc);
  return rc;
}
