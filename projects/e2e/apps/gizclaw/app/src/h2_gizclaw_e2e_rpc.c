#include "h2_gizclaw_e2e_rpc.h"
#include "h2_gizclaw_e2e_report.h"
#include "h2_gizclaw_service.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_E2E_SPEED_BYTES (256u * 1024u)
#define H2_GIZCLAW_E2E_UPLOAD_MIN_BPS 40000u
#define H2_GIZCLAW_E2E_DOWNLOAD_MIN_BPS 200000u

static int checked(const char *symbol, const char *stage, int result) {
  h2_gizclaw_e2e_evidence(symbol, stage, result);
  return result;
}

static int append_run_suffix(char *destination, size_t capacity,
                             const char *run_prefix, const char *suffix) {
  if (destination == NULL || capacity == 0u || run_prefix == NULL ||
      suffix == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t prefix_len = strlen(run_prefix);
  const size_t suffix_len = strlen(suffix);
  if (prefix_len >= capacity || suffix_len >= capacity - prefix_len)
    return H2_PAL_ERR_FORMAT;
  memcpy(destination, run_prefix, prefix_len);
  memcpy(destination + prefix_len, suffix, suffix_len + 1u);
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_select_workflow_name(
    const h2_gizclaw_workflow_page_t *workflows, char *out_name,
    size_t out_name_capacity) {
  if (workflows == NULL || out_name == NULL || out_name_capacity == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_name[0] = '\0';
  if (workflows->count > 0u && workflows->items == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const char *selected = NULL;
  for (size_t index = 0u; index < workflows->count; ++index) {
    const char *candidate = workflows->items[index].name;
    if (candidate != NULL && candidate[0] != '\0' &&
        (selected == NULL || strcmp(candidate, selected) < 0)) {
      selected = candidate;
    }
  }
  if (selected == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  const size_t len = strlen(selected);
  if (len >= out_name_capacity) {
    return H2_PAL_ERR_TRUNCATED;
  }
  memcpy(out_name, selected, len + 1u);
  return H2_PAL_OK;
}

bool h2_gizclaw_e2e_workspace_response_ready(
    const h2_gizclaw_workspace_t *workspace, const char *expected_name) {
  return workspace != NULL && workspace->available && workspace->name != NULL &&
         expected_name != NULL && strcmp(workspace->name, expected_name) == 0;
}

typedef struct connectivity_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_service_t *service;
  atomic_bool complete;
  atomic_int result;
  h2_gizclaw_ping_result_t ping;
  h2_gizclaw_speedtest_result_t speed;
} connectivity_state_t;

static void connectivity_ping_complete(
    void *user, h2_gizclaw_ping_request_t *request) {
  connectivity_state_t *state = user;
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_ping_request_operation_result(request);
  const h2_gizclaw_ping_result_t *ping =
      h2_gizclaw_ping_request_response(request);
  int rc = result == NULL ? H2_PAL_ERR_INVALID_STATE : result->result;
  if (rc == H2_PAL_OK && ping == NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    state->ping = *ping;
  atomic_store_explicit(&state->result, rc, memory_order_release);
  atomic_store_explicit(&state->complete, true, memory_order_release);
  h2_gizclaw_ping_request_release(request);
}

static void connectivity_speed_complete(
    void *user, h2_gizclaw_speedtest_request_t *request) {
  connectivity_state_t *state = user;
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_speedtest_request_operation_result(request);
  const h2_gizclaw_speedtest_result_t *speedtest =
      h2_gizclaw_speedtest_request_response(request);
  int rc = result == NULL ? H2_PAL_ERR_INVALID_STATE : result->result;
  if (rc == H2_PAL_OK && speedtest == NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    state->speed = *speedtest;
  atomic_store_explicit(&state->result, rc, memory_order_release);
  atomic_store_explicit(&state->complete, true, memory_order_release);
  h2_gizclaw_speedtest_request_release(request);
}

static void connectivity_telemetry_complete(
    void *user, h2_gizclaw_telemetry_request_t *request) {
  connectivity_state_t *state = user;
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_telemetry_request_operation_result(request);
  const int rc = result == NULL ? H2_PAL_ERR_INVALID_STATE : result->result;
  atomic_store_explicit(&state->result, rc, memory_order_release);
  atomic_store_explicit(&state->complete, true, memory_order_release);
  h2_gizclaw_telemetry_request_release(request);
}

static int connectivity_wait(connectivity_state_t *state) {
  while (!atomic_load_explicit(&state->complete, memory_order_acquire)) {
    size_t dispatched = 0u;
    const int dispatch_rc =
        h2_gizclaw_service_dispatch(state->service, 8u, &dispatched);
    if (dispatch_rc != H2_PAL_OK)
      return dispatch_rc;
    if (!h2_gizclaw_e2e_fixture_has_time(state->fixture, 10u))
      return H2_PAL_ERR_TIMEOUT;
    const int rc = h2_pal_time_sleep_ms(state->fixture->time, 10u);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return atomic_load_explicit(&state->result, memory_order_acquire);
}

static void connectivity_reset(connectivity_state_t *state) {
  memset(&state->speed, 0, sizeof(state->speed));
  atomic_store_explicit(&state->result, H2_PAL_OK, memory_order_relaxed);
  atomic_store_explicit(&state->complete, false, memory_order_release);
}

int h2_gizclaw_e2e_run_connectivity(h2_gizclaw_e2e_fixture_t *fixture) {
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
  connectivity_state_t state;
  memset(&state, 0, sizeof(state));
  state.fixture = fixture;
  atomic_init(&state.complete, false);
  atomic_init(&state.result, H2_PAL_OK);
  const h2_gizclaw_service_config_t config = {
      .client_config = &client_config,
      .task = fixture->runtime->task,
      .queue = fixture->runtime->queue,
      .sync = fixture->runtime->sync,
      .net_task_options = {.min_stack_size = 32768u},
      .operation_capacity = 1u,
      .client_poll_timeout_ms = 10,
  };
  rc = h2_gizclaw_service_init(&config, &state.service);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_service_start(state.service);

  h2_gizclaw_ping_request_t *ping_request = NULL;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_ping_async(state.service, 1u, 30000u,
                                       connectivity_ping_complete, &state,
                                       &ping_request);
  }
  if (rc == H2_PAL_OK)
    rc = connectivity_wait(&state);
  rc = checked("h2_gizclaw_service_ping_async", "core", rc);

  h2_gizclaw_speedtest_request_t *speed_request = NULL;
  connectivity_reset(&state);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_speedtest_async(
        state.service, 2u, H2_GIZCLAW_E2E_SPEED_BYTES, 0u, 30000u,
        connectivity_speed_complete, &state, &speed_request);
  }
  if (rc == H2_PAL_OK)
    rc = connectivity_wait(&state);
  if (rc == H2_PAL_OK &&
      (state.speed.upload_bytes != H2_GIZCLAW_E2E_SPEED_BYTES ||
       state.speed.upload_bits_per_second < H2_GIZCLAW_E2E_UPLOAD_MIN_BPS)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  rc = checked("h2_gizclaw_service_speedtest_async", "diagnostics", rc);
  printf("H2_GIZCLAW_E2E stage=upload result=%s bytes=%" PRIu64 " bps=%" PRIu64
         " attempts=%u\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", state.speed.upload_bytes,
         state.speed.upload_bits_per_second, 1u);

  connectivity_reset(&state);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_speedtest_async(
        state.service, 3u, 0u, H2_GIZCLAW_E2E_SPEED_BYTES, 30000u,
        connectivity_speed_complete, &state, &speed_request);
  }
  if (rc == H2_PAL_OK)
    rc = connectivity_wait(&state);
  if (rc == H2_PAL_OK &&
      (state.speed.download_bytes != H2_GIZCLAW_E2E_SPEED_BYTES ||
       state.speed.download_bits_per_second < H2_GIZCLAW_E2E_DOWNLOAD_MIN_BPS)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  rc = checked("h2_gizclaw_service_speedtest_async", "diagnostics", rc);
  printf("H2_GIZCLAW_E2E stage=download result=%s bytes=%" PRIu64
         " bps=%" PRIu64 " attempts=%u\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", state.speed.download_bytes,
         state.speed.download_bits_per_second, 1u);
  const int stop_rc = state.service == NULL
                          ? H2_PAL_OK
                          : h2_gizclaw_service_stop(state.service);
  if (state.service != NULL) {
    for (;;) {
      size_t dispatched = 0u;
      const int dispatch_rc =
          h2_gizclaw_service_dispatch(state.service, 8u, &dispatched);
      if (rc == H2_PAL_OK)
        rc = dispatch_rc;
      if (dispatch_rc != H2_PAL_OK || dispatched == 0u)
        break;
    }
  }
  const int deinit_rc = state.service == NULL
                            ? H2_PAL_OK
                            : h2_gizclaw_service_deinit(state.service);
  if (rc == H2_PAL_OK)
    rc = stop_rc;
  if (rc == H2_PAL_OK)
    rc = deinit_rc;
  return rc;
}

static int run_profile(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_profile_t profile = {0};
  int rc = checked("h2_gizclaw_client_profile_get", "profile",
                   h2_gizclaw_client_profile_get(client, &profile));
  if (rc != H2_PAL_OK)
    return rc;
  char name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(name, sizeof(name), fixture->run_prefix, "-owner");
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_profile_put_name", "profile",
               h2_gizclaw_client_profile_put_name(
                   client, h2_gizclaw_e2e_str(name), &profile));
  if (rc != H2_PAL_OK || !profile.has_name || strcmp(profile.name, name) != 0)
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  const h2_gizclaw_str_t emoji = {.data = "🤖", .len = 4u};
  rc = checked("h2_gizclaw_client_profile_put_emoji", "profile",
               h2_gizclaw_client_profile_put_emoji(client, emoji, &profile));
  if (rc == H2_PAL_OK &&
      (!profile.has_emoji || strcmp(profile.emoji, "🤖") != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static int run_catalog_workspace(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_workflow_page_t workflows = {0};
  int rc = checked(
      "h2_gizclaw_client_workflows_list", "catalog",
      h2_gizclaw_client_workflows_list(client, h2_gizclaw_e2e_str("assistants"),
                                       (h2_gizclaw_str_t){0}, 32u, &workflows));
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_e2e_select_workflow_name(&workflows, fixture->workflow_name,
                                             sizeof(fixture->workflow_name));
  }
  h2_gizclaw_workflow_page_deinit(client, &workflows);
  if (rc != H2_PAL_OK)
    return rc;
  printf("H2_GIZCLAW_E2E stage=catalog selected_workflow=%s\n",
         fixture->workflow_name);

  h2_gizclaw_workflow_t workflow = {0};
  char *profile_name = NULL;
  char *profile_revision = NULL;
  rc = checked("h2_gizclaw_client_workflow_get", "catalog",
               h2_gizclaw_client_workflow_get(
                   client, h2_gizclaw_e2e_str(fixture->workflow_name),
                   &workflow, &profile_name, &profile_revision));
  if (rc == H2_PAL_OK &&
      (workflow.name == NULL ||
       strcmp(workflow.name, fixture->workflow_name) != 0 ||
       profile_name == NULL ||
       strcmp(profile_name, fixture->runtime_profile_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workflow_get_deinit(client, &workflow, profile_name,
                                 profile_revision);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_workspace_page_t workspaces = {0};
  rc = checked("h2_gizclaw_client_workspaces_list", "workspace",
               h2_gizclaw_client_workspaces_list(
                   client, h2_gizclaw_e2e_str("assistants"),
                   (h2_gizclaw_str_t){0}, 32u, &workspaces));
  h2_gizclaw_workspace_page_deinit(client, &workspaces);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_workspace_t workspace = {0};
  if (!fixture->workspace_created) {
    rc = checked("h2_gizclaw_client_workspace_create", "workspace",
                 h2_gizclaw_client_workspace_create(
                     client, h2_gizclaw_e2e_str("assistants"),
                     h2_gizclaw_e2e_str(fixture->workflow_name),
                     h2_gizclaw_e2e_str(fixture->workspace_name), &workspace));
    if (rc == H2_PAL_OK &&
        (workspace.name == NULL ||
         strcmp(workspace.name, fixture->workspace_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_OK)
      fixture->workspace_created = true;
    h2_gizclaw_workspace_deinit(client, &workspace);
    if (rc != H2_PAL_OK)
      return rc;
  }

  profile_name = NULL;
  profile_revision = NULL;
  rc = checked("h2_gizclaw_client_workspace_get", "workspace",
               h2_gizclaw_client_workspace_get(
                   client, h2_gizclaw_e2e_str(fixture->workspace_name),
                   &workspace, &profile_name, &profile_revision));
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0 ||
       profile_name == NULL ||
       strcmp(profile_name, fixture->runtime_profile_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_deinit(client, &workspace);
  h2_pal_mem_free(fixture->allocator, profile_name);
  h2_pal_mem_free(fixture->allocator, profile_revision);
  if (rc != H2_PAL_OK)
    return rc;

  rc = checked("h2_gizclaw_client_workspace_set_input", "workspace",
               h2_gizclaw_client_workspace_set_input(
                   client, h2_gizclaw_e2e_str(fixture->workspace_name),
                   H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK, &workspace));
  if (rc == H2_PAL_OK && !h2_gizclaw_e2e_workspace_response_ready(
                             &workspace, fixture->workspace_name)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_deinit(client, &workspace);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_workspace_activation_t activation = {0};
  rc = checked(
      "h2_gizclaw_client_workspace_activate", "workspace",
      h2_gizclaw_client_workspace_activate(
          client, h2_gizclaw_e2e_str(fixture->workspace_name), &activation));
  if (rc == H2_PAL_OK && !h2_gizclaw_workspace_activation_ready(
                             &activation, fixture->workspace_name)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_activation_deinit(client, &activation);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_workspace_history_page_t history = {0};
  rc = checked("h2_gizclaw_client_workspace_history_list", "history",
               h2_gizclaw_client_workspace_history_list(
                   client, h2_gizclaw_e2e_str(fixture->workspace_name),
                   (h2_gizclaw_str_t){0}, 32u,
                   H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC, &history));
  h2_gizclaw_workspace_history_page_deinit(client, &history);
  return rc;
}

static int run_workspace_reconnect(h2_gizclaw_e2e_fixture_t *fixture) {
  int rc =
      h2_gizclaw_e2e_fixture_reconnect_actor(fixture, H2_GIZCLAW_E2E_OWNER);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_workspace_t workspace = {0};
  char *profile_name = NULL;
  char *profile_revision = NULL;
  rc = h2_gizclaw_client_workspace_get(
      client, h2_gizclaw_e2e_str(fixture->workspace_name), &workspace,
      &profile_name, &profile_revision);
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_deinit(client, &workspace);
  h2_pal_mem_free(fixture->allocator, profile_name);
  h2_pal_mem_free(fixture->allocator, profile_revision);
  h2_gizclaw_workspace_history_page_t history = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_workspace_history_list(
        client, h2_gizclaw_e2e_str(fixture->workspace_name),
        (h2_gizclaw_str_t){0}, 32u, H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC,
        &history);
  }
  h2_gizclaw_workspace_history_page_deinit(client, &history);
  printf("H2_GIZCLAW_E2E stage=workspace_reconnect result=%s\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL");
  return rc;
}

static int run_contact(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  char resource_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  int rc = append_run_suffix(resource_name, sizeof(resource_name),
                             fixture->run_prefix, "-contact");
  if (rc != H2_PAL_OK)
    return rc;
  char display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(display_name, sizeof(display_name),
                         fixture->run_prefix, " contact");
  if (rc != H2_PAL_OK)
    return rc;
  char updated_display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(updated_display_name, sizeof(updated_display_name),
                         fixture->run_prefix, "-updated");
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t contact = {0};
  rc = checked("h2_gizclaw_client_contact_create", "contact",
               h2_gizclaw_client_contact_create(
                   client, h2_gizclaw_e2e_str(resource_name),
                   h2_gizclaw_e2e_str(display_name),
                   h2_gizclaw_e2e_str("+8613900000644"), &contact));
  if (rc == H2_PAL_OK && contact.name != NULL) {
    (void)snprintf(fixture->contact_name, sizeof(fixture->contact_name), "%s",
                   contact.name);
    fixture->contact_created = true;
  } else if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_FORMAT;
  }
  h2_gizclaw_contact_deinit(client, &contact);
  if (rc != H2_PAL_OK)
    return rc;

  rc =
      checked("h2_gizclaw_client_contact_get", "contact",
              h2_gizclaw_client_contact_get(
                  client, h2_gizclaw_e2e_str(fixture->contact_name), &contact));
  h2_gizclaw_contact_deinit(client, &contact);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_page_t page = {0};
  rc = checked("h2_gizclaw_client_contacts_list", "contact",
               h2_gizclaw_client_contacts_list(client, (h2_gizclaw_str_t){0},
                                               32u, &page));
  h2_gizclaw_contact_page_deinit(client, &page);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_contact_put", "contact",
               h2_gizclaw_client_contact_put(
                   client, h2_gizclaw_e2e_str(fixture->contact_name),
                   h2_gizclaw_e2e_str(updated_display_name),
                   h2_gizclaw_e2e_str("+8613900000644"), &contact));
  h2_gizclaw_contact_deinit(client, &contact);
  return rc;
}

static int run_friend(h2_gizclaw_e2e_fixture_t *fixture) {
  int rc = H2_PAL_OK;
  h2_gizclaw_client_t *owner = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_client_t *friend_client =
      fixture->actors[H2_GIZCLAW_E2E_FRIEND].client;
  h2_gizclaw_invite_token_t token = {0};
  rc = checked(
      "h2_gizclaw_client_friend_invite_token_create", "friend",
      h2_gizclaw_client_friend_invite_token_create(friend_client, &token));
  if (rc == H2_PAL_OK)
    fixture->friend_invite_created = true;
  if (rc != H2_PAL_OK || token.value == NULL) {
    h2_gizclaw_invite_token_deinit(friend_client, &token);
    return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
  }
  char token_value[512u];
  const int token_len =
      snprintf(token_value, sizeof(token_value), "%s", token.value);
  h2_gizclaw_invite_token_deinit(friend_client, &token);
  if (token_len <= 0 || (size_t)token_len >= sizeof(token_value))
    return H2_PAL_ERR_FORMAT;
  rc =
      checked("h2_gizclaw_client_friend_invite_token_get", "friend",
              h2_gizclaw_client_friend_invite_token_get(friend_client, &token));
  h2_gizclaw_invite_token_deinit(friend_client, &token);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_t friend_value = {0};
  rc = checked("h2_gizclaw_client_friend_add", "friend",
               h2_gizclaw_client_friend_add(
                   owner, h2_gizclaw_e2e_str(token_value), &friend_value));
  if (rc == H2_PAL_OK && friend_value.id != NULL) {
    (void)snprintf(fixture->friend_id, sizeof(fixture->friend_id), "%s",
                   friend_value.id);
    fixture->friendship_created = true;
  } else if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_FORMAT;
  }
  h2_gizclaw_friend_deinit(owner, &friend_value);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_friend_invite_token_clear", "friend",
               h2_gizclaw_client_friend_invite_token_clear(friend_client));
  if (rc == H2_PAL_OK)
    fixture->friend_invite_created = false;
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_page_t friends = {0};
  rc = checked("h2_gizclaw_client_friends_list", "friend",
               h2_gizclaw_client_friends_list(owner, (h2_gizclaw_str_t){0}, 32u,
                                              &friends));
  h2_gizclaw_friend_page_deinit(owner, &friends);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked(
      "h2_gizclaw_client_friend_info_get", "friend",
      h2_gizclaw_client_friend_info_get(
          owner, h2_gizclaw_e2e_str(fixture->friend_id), &friend_value));
  h2_gizclaw_friend_deinit(owner, &friend_value);
  return rc;
}

typedef struct group_audio_counter {
  uint64_t count;
} group_audio_counter_t;

static int count_group_audio(void *user, const uint8_t *data, size_t len) {
  group_audio_counter_t *counter = user;
  if (counter == NULL || data == NULL || len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  counter->count += len;
  return H2_PAL_OK;
}

static int run_group(h2_gizclaw_e2e_fixture_t *fixture) {
  int rc = H2_PAL_OK;
  h2_gizclaw_client_t *owner = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_client_t *member_client =
      fixture->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].client;
  char group_resource_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(group_resource_name, sizeof(group_resource_name),
                         fixture->run_prefix, "-group");
  if (rc != H2_PAL_OK)
    return rc;
  char group_display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(group_display_name, sizeof(group_display_name),
                         fixture->run_prefix, " group");
  if (rc != H2_PAL_OK)
    return rc;
  char renamed_display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(renamed_display_name, sizeof(renamed_display_name),
                         fixture->run_prefix, " renamed");
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t group = {0};
  rc = checked("h2_gizclaw_client_friend_group_create", "group",
               h2_gizclaw_client_friend_group_create(
                   owner, h2_gizclaw_e2e_str(group_resource_name),
                   h2_gizclaw_e2e_str(group_display_name),
                   (h2_gizclaw_str_t){0}, &group));
  if (rc == H2_PAL_OK && group.name != NULL &&
      strcmp(group.name, group_resource_name) == 0 &&
      group.workspace_name != NULL) {
    (void)snprintf(fixture->friend_group_name,
                   sizeof(fixture->friend_group_name), "%s", group.name);
    (void)snprintf(fixture->friend_group_workspace_name,
                   sizeof(fixture->friend_group_workspace_name), "%s",
                   group.workspace_name);
    fixture->friend_group_created = true;
  } else if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_FORMAT;
  }
  h2_gizclaw_friend_group_deinit(owner, &group);
  if (rc != H2_PAL_OK)
    return rc;

  rc = checked(
      "h2_gizclaw_client_friend_group_get", "group",
      h2_gizclaw_client_friend_group_get(
          owner, h2_gizclaw_e2e_str(fixture->friend_group_name), &group));
  h2_gizclaw_friend_group_deinit(owner, &group);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_page_t groups = {0};
  rc = checked("h2_gizclaw_client_friend_groups_list", "group",
               h2_gizclaw_client_friend_groups_list(
                   owner, (h2_gizclaw_str_t){0}, 32u, &groups));
  h2_gizclaw_friend_group_page_deinit(owner, &groups);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_friend_group_put", "group",
               h2_gizclaw_client_friend_group_put(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                   h2_gizclaw_e2e_str(renamed_display_name),
                   (h2_gizclaw_str_t){0}, &group));
  h2_gizclaw_friend_group_deinit(owner, &group);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_invite_token_t token = {0};
  rc = checked(
      "h2_gizclaw_client_friend_group_invite_token_create", "group",
      h2_gizclaw_client_friend_group_invite_token_create(
          owner, h2_gizclaw_e2e_str(fixture->friend_group_name), &token));
  if (rc == H2_PAL_OK)
    fixture->friend_group_invite_created = true;
  if (rc != H2_PAL_OK || token.value == NULL) {
    h2_gizclaw_invite_token_deinit(owner, &token);
    return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
  }
  char token_value[512u];
  const int token_len =
      snprintf(token_value, sizeof(token_value), "%s", token.value);
  h2_gizclaw_invite_token_deinit(owner, &token);
  if (token_len <= 0 || (size_t)token_len >= sizeof(token_value))
    return H2_PAL_ERR_FORMAT;
  rc = checked(
      "h2_gizclaw_client_friend_group_invite_token_get", "group",
      h2_gizclaw_client_friend_group_invite_token_get(
          owner, h2_gizclaw_e2e_str(fixture->friend_group_name), &token));
  h2_gizclaw_invite_token_deinit(owner, &token);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_friend_group_join", "group",
               h2_gizclaw_client_friend_group_join(
                   member_client, h2_gizclaw_e2e_str(token_value),
                   h2_gizclaw_e2e_str(group_resource_name), &group));
  if (rc == H2_PAL_OK &&
      (group.name == NULL || strcmp(group.name, group_resource_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK)
    fixture->friend_group_member_joined = true;
  h2_gizclaw_friend_group_deinit(member_client, &group);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_friend_group_invite_token_clear", "group",
               h2_gizclaw_client_friend_group_invite_token_clear(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name)));
  if (rc == H2_PAL_OK)
    fixture->friend_group_invite_created = false;
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_friend_group_member_page_t members = {0};
  rc = checked("h2_gizclaw_client_friend_group_members_list", "group",
               h2_gizclaw_client_friend_group_members_list(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                   (h2_gizclaw_str_t){0}, 32u, &members));
  char member_id[H2_GIZCLAW_E2E_NAME_CAPACITY] = {0};
  if (rc == H2_PAL_OK) {
    for (size_t index = 0u; index < members.count; ++index) {
      if (members.items[index].peer_public_key != NULL &&
          strcmp(members.items[index].peer_public_key,
                 fixture->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key) ==
              0 &&
          members.items[index].id != NULL) {
        (void)snprintf(member_id, sizeof(member_id), "%s",
                       members.items[index].id);
      }
    }
  }
  h2_gizclaw_friend_group_member_page_deinit(owner, &members);
  if (rc != H2_PAL_OK || member_id[0] == '\0')
    return rc == H2_PAL_OK ? H2_PAL_ERR_NOT_FOUND : rc;
  (void)snprintf(fixture->friend_group_member_id,
                 sizeof(fixture->friend_group_member_id), "%s", member_id);

  h2_gizclaw_friend_group_member_t member = {0};
  rc = checked("h2_gizclaw_client_friend_group_member_put", "group",
               h2_gizclaw_client_friend_group_member_put(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                   h2_gizclaw_e2e_str(member_id),
                   H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN, &member));
  h2_gizclaw_friend_group_member_deinit(owner, &member);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_friend_group_message_page_t messages = {0};
  rc = checked("h2_gizclaw_client_friend_group_messages_list", "group",
               h2_gizclaw_client_friend_group_messages_list(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                   (h2_gizclaw_str_t){0}, 32u, &messages));
  char history_id[H2_GIZCLAW_E2E_NAME_CAPACITY] = {0};
  bool audio_available = false;
  if (rc == H2_PAL_OK && messages.count > 0u &&
      messages.items[0].history_id != NULL) {
    (void)snprintf(history_id, sizeof(history_id), "%s",
                   messages.items[0].history_id);
    audio_available = messages.items[0].audio_available;
  }
  h2_gizclaw_friend_group_message_page_deinit(owner, &messages);
  if (rc != H2_PAL_OK)
    return rc;
  if (history_id[0] != '\0') {
    h2_gizclaw_friend_group_message_t message = {0};
    rc = checked("h2_gizclaw_client_friend_group_message_get", "group",
                 h2_gizclaw_client_friend_group_message_get(
                     owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                     h2_gizclaw_e2e_str(history_id), &message));
    h2_gizclaw_friend_group_message_deinit(owner, &message);
    if (rc != H2_PAL_OK)
      return rc;
  }
  if (history_id[0] != '\0' && audio_available) {
    group_audio_counter_t audio = {0};
    h2_gizclaw_friend_group_message_audio_info_t info = {0};
    rc = checked("h2_gizclaw_client_friend_group_message_audio_download", "group",
                 h2_gizclaw_client_friend_group_message_audio_download(
                     owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                     h2_gizclaw_e2e_str(history_id), count_group_audio, &audio,
                     &info));
    if (rc == H2_PAL_OK &&
        (info.friend_group_name == NULL || info.history_id == NULL ||
         strcmp(info.friend_group_name, fixture->friend_group_name) != 0 ||
         strcmp(info.history_id, history_id) != 0 || audio.count == 0u ||
         audio.count != info.received_bytes ||
         info.received_bytes != info.size_bytes)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_gizclaw_friend_group_message_audio_info_deinit(owner, &info);
    if (rc != H2_PAL_OK)
      return rc;
  }
  rc = checked("h2_gizclaw_client_friend_group_member_delete", "group",
               h2_gizclaw_client_friend_group_member_delete(
                   owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
                   h2_gizclaw_e2e_str(member_id), &member));
  h2_gizclaw_friend_group_member_deinit(owner, &member);
  if (rc == H2_PAL_OK)
    fixture->friend_group_member_joined = false;
  return rc;
}

typedef struct byte_counter {
  uint64_t count;
} byte_counter_t;

static int count_bytes(void *user, const uint8_t *data, size_t len) {
  byte_counter_t *counter = user;
  if (counter == NULL || data == NULL || len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  counter->count += len;
  return H2_PAL_OK;
}

static int run_gameplay(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_pet_page_t pets = {0};
  int rc = checked(
      "h2_gizclaw_client_pet_list", "gameplay",
      h2_gizclaw_client_pet_list(client, (h2_gizclaw_str_t){0}, 32u, &pets));
  h2_gizclaw_pet_page_deinit(client, &pets);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_pet_adopt_options_t adopt = {
      .name = h2_gizclaw_e2e_str(fixture->pet_name),
      .display_name = h2_gizclaw_e2e_str("H2 E2E Pet"),
  };
  h2_gizclaw_pet_t pet = {0};
  rc = checked("h2_gizclaw_client_pet_adopt", "gameplay",
               h2_gizclaw_client_pet_adopt(client, &adopt, &pet));
  if (rc == H2_PAL_OK &&
      (pet.name == NULL || strcmp(pet.name, fixture->pet_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK)
    fixture->pet_created = true;
  h2_gizclaw_pet_deinit(client, &pet);
  if (rc != H2_PAL_OK)
    return rc;
  rc = checked("h2_gizclaw_client_pet_get", "gameplay",
               h2_gizclaw_client_pet_get(
                   client, h2_gizclaw_e2e_str(fixture->pet_name), &pet));
  h2_gizclaw_pet_deinit(client, &pet);
  if (rc != H2_PAL_OK)
    return rc;
  char drive_key[H2_GIZCLAW_E2E_NAME_CAPACITY];
  rc = append_run_suffix(drive_key, sizeof(drive_key), fixture->run_prefix,
                         "-drive");
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_pet_drive_options_t drive = {
      .pet_name = h2_gizclaw_e2e_str(fixture->pet_name),
      .behavior = H2_GIZCLAW_PET_BEHAVIOR_NONE,
      .idempotency_key = h2_gizclaw_e2e_str(drive_key),
  };
  rc = checked("h2_gizclaw_client_pet_drive", "gameplay",
               h2_gizclaw_client_pet_drive(client, &drive, &pet));
  h2_gizclaw_pet_deinit(client, &pet);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_pet_actions_t actions = {0};
  rc = checked("h2_gizclaw_client_pet_actions_get", "gameplay",
               h2_gizclaw_client_pet_actions_get(
                   client, h2_gizclaw_e2e_str(fixture->pet_name), &actions));
  h2_gizclaw_pet_actions_deinit(client, &actions);
  if (rc != H2_PAL_OK)
    return rc;
  byte_counter_t pixa = {0};
  h2_gizclaw_pet_pixa_info_t pixa_info = {0};
  rc = checked("h2_gizclaw_client_pet_pixa_download", "gameplay",
               h2_gizclaw_client_pet_pixa_download(
                   client, h2_gizclaw_e2e_str(fixture->pet_name), count_bytes,
                   &pixa, &pixa_info));
  if (rc == H2_PAL_OK &&
      (pixa.count == 0u || pixa.count != pixa_info.received_bytes ||
       pixa_info.received_bytes != pixa_info.size_bytes)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_pet_pixa_info_deinit(client, &pixa_info);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_points_account_t account = {0};
  rc = checked("h2_gizclaw_client_points_get", "gameplay",
               h2_gizclaw_client_points_get(client, &account));
  h2_gizclaw_points_account_deinit(client, &account);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_points_transaction_page_t transactions = {0};
  rc = checked("h2_gizclaw_client_points_transactions_list", "gameplay",
               h2_gizclaw_client_points_transactions_list(
                   client, (h2_gizclaw_str_t){0}, 32u, &transactions));
  h2_gizclaw_points_transaction_page_deinit(client, &transactions);
  return rc;
}

static void keep_first_failure(int candidate, int *result) {
  if (*result == H2_PAL_OK && candidate != H2_PAL_OK)
    *result = candidate;
}

static int run_peer_name_isolation(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_FRIEND].client;
  if (client == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  char peer_display_name[H2_GIZCLAW_E2E_NAME_CAPACITY];
  int rc = append_run_suffix(peer_display_name, sizeof(peer_display_name),
                             fixture->run_prefix, " peer-b");
  if (rc != H2_PAL_OK)
    return rc;
  bool workspace_created = false;
  bool contact_created = false;
  bool group_created = false;
  bool pet_created = false;
  int result = H2_PAL_OK;

  h2_gizclaw_workspace_t workspace = {0};
  rc = h2_gizclaw_client_workspace_create(
      client, h2_gizclaw_e2e_str("assistants"),
      h2_gizclaw_e2e_str(fixture->workflow_name),
      h2_gizclaw_e2e_str(fixture->workspace_name), &workspace);
  workspace_created = rc == H2_PAL_OK;
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_deinit(client, &workspace);
  if (rc == H2_PAL_OK) {
    char *profile_name = NULL;
    char *profile_revision = NULL;
    rc = h2_gizclaw_client_workspace_get(
        client, h2_gizclaw_e2e_str(fixture->workspace_name), &workspace,
        &profile_name, &profile_revision);
    if (rc == H2_PAL_OK &&
        (workspace.name == NULL ||
         strcmp(workspace.name, fixture->workspace_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_gizclaw_workspace_deinit(client, &workspace);
    h2_pal_mem_free(fixture->allocator, profile_name);
    h2_pal_mem_free(fixture->allocator, profile_revision);
  }
  keep_first_failure(rc, &result);

  h2_gizclaw_contact_t contact = {0};
  if (result == H2_PAL_OK) {
    rc = h2_gizclaw_client_contact_create(
        client, h2_gizclaw_e2e_str(fixture->contact_name),
        h2_gizclaw_e2e_str(peer_display_name),
        h2_gizclaw_e2e_str("+8613900000690"), &contact);
    contact_created = rc == H2_PAL_OK;
    if (rc == H2_PAL_OK &&
        (contact.name == NULL || contact.display_name == NULL ||
         strcmp(contact.name, fixture->contact_name) != 0 ||
         strcmp(contact.display_name, peer_display_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_gizclaw_contact_deinit(client, &contact);
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_client_contact_get(
          client, h2_gizclaw_e2e_str(fixture->contact_name), &contact);
      if (rc == H2_PAL_OK &&
          (contact.name == NULL || contact.display_name == NULL ||
           strcmp(contact.name, fixture->contact_name) != 0 ||
           strcmp(contact.display_name, peer_display_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      h2_gizclaw_contact_deinit(client, &contact);
    }
    keep_first_failure(rc, &result);
  }

  h2_gizclaw_friend_group_t group = {0};
  if (result == H2_PAL_OK) {
    rc = h2_gizclaw_client_friend_group_create(
        client, h2_gizclaw_e2e_str(fixture->friend_group_name),
        h2_gizclaw_e2e_str(peer_display_name), (h2_gizclaw_str_t){0}, &group);
    group_created = rc == H2_PAL_OK;
    if (rc == H2_PAL_OK &&
        (group.name == NULL || group.display_name == NULL ||
         strcmp(group.name, fixture->friend_group_name) != 0 ||
         strcmp(group.display_name, peer_display_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_gizclaw_friend_group_deinit(client, &group);
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_client_friend_group_get(
          client, h2_gizclaw_e2e_str(fixture->friend_group_name), &group);
      if (rc == H2_PAL_OK &&
          (group.name == NULL || group.display_name == NULL ||
           strcmp(group.name, fixture->friend_group_name) != 0 ||
           strcmp(group.display_name, peer_display_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      h2_gizclaw_friend_group_deinit(client, &group);
    }
    keep_first_failure(rc, &result);
  }

  h2_gizclaw_pet_t pet = {0};
  if (result == H2_PAL_OK) {
    const h2_gizclaw_pet_adopt_options_t options = {
        .name = h2_gizclaw_e2e_str(fixture->pet_name),
        .display_name = h2_gizclaw_e2e_str(peer_display_name),
    };
    rc = h2_gizclaw_client_pet_adopt(client, &options, &pet);
    pet_created = rc == H2_PAL_OK;
    if (rc == H2_PAL_OK &&
        (pet.name == NULL || strcmp(pet.name, fixture->pet_name) != 0)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_gizclaw_pet_deinit(client, &pet);
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_client_pet_get(
          client, h2_gizclaw_e2e_str(fixture->pet_name), &pet);
      if (rc == H2_PAL_OK &&
          (pet.name == NULL || strcmp(pet.name, fixture->pet_name) != 0)) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
      h2_gizclaw_pet_deinit(client, &pet);
    }
    keep_first_failure(rc, &result);
  }

  if (pet_created) {
    rc = h2_gizclaw_client_pet_delete(
        client, h2_gizclaw_e2e_str(fixture->pet_name), &pet);
    h2_gizclaw_pet_deinit(client, &pet);
    keep_first_failure(rc, &result);
  }
  if (group_created) {
    rc = h2_gizclaw_client_friend_group_delete(
        client, h2_gizclaw_e2e_str(fixture->friend_group_name), &group);
    h2_gizclaw_friend_group_deinit(client, &group);
    keep_first_failure(rc, &result);
  }
  if (contact_created) {
    rc = h2_gizclaw_client_contact_delete(
        client, h2_gizclaw_e2e_str(fixture->contact_name), &contact);
    h2_gizclaw_contact_deinit(client, &contact);
    keep_first_failure(rc, &result);
  }
  if (workspace_created) {
    rc = h2_gizclaw_client_workspace_delete(
        client, h2_gizclaw_e2e_str(fixture->workspace_name), &workspace);
    h2_gizclaw_workspace_deinit(client, &workspace);
    keep_first_failure(rc, &result);
  }
  printf("H2_GIZCLAW_E2E stage=peer_name_isolation result=%s\n",
         result == H2_PAL_OK ? "PASS" : "FAIL");
  return result;
}

static bool contains_blue(const char *text) {
  if (text == NULL)
    return false;
  const size_t len = strlen(text);
  for (size_t index = 0u; index + 4u <= len; ++index) {
    const char *cursor = text + index;
    if ((cursor[0] == 'b' || cursor[0] == 'B') &&
        (cursor[1] == 'l' || cursor[1] == 'L') &&
        (cursor[2] == 'u' || cursor[2] == 'U') &&
        (cursor[3] == 'e' || cursor[3] == 'E')) {
      return true;
    }
  }
  return strstr(text, "蓝") != NULL;
}

typedef struct extraction_result {
  bool transcript_has_marker;
  bool json_has_marker;
} extraction_result_t;

typedef enum speech_failure_stage {
  SPEECH_FAILURE_TRANSCRIBE_OPEN = 0,
  SPEECH_FAILURE_TRANSCRIBE_WRITE,
  SPEECH_FAILURE_TRANSCRIBE_FINISH,
  SPEECH_FAILURE_TRANSCRIBE_VALIDATE,
  SPEECH_FAILURE_EXTRACT_OPEN,
  SPEECH_FAILURE_EXTRACT_WRITE,
  SPEECH_FAILURE_EXTRACT_FINISH,
  SPEECH_FAILURE_EXTRACT_VALIDATE,
} speech_failure_stage_t;

static const char *speech_failure_symbol(speech_failure_stage_t stage) {
  switch (stage) {
  case SPEECH_FAILURE_TRANSCRIBE_OPEN:
    return "h2_gizclaw_client_speech_transcribe_open";
  case SPEECH_FAILURE_TRANSCRIBE_WRITE:
    return "h2_gizclaw_speech_transcribe_write";
  case SPEECH_FAILURE_TRANSCRIBE_FINISH:
    return "h2_gizclaw_speech_transcribe_finish";
  case SPEECH_FAILURE_TRANSCRIBE_VALIDATE:
    return "speech_transcribe_validate";
  case SPEECH_FAILURE_EXTRACT_OPEN:
    return "h2_gizclaw_client_speech_extract_open";
  case SPEECH_FAILURE_EXTRACT_WRITE:
    return "h2_gizclaw_speech_extract_write";
  case SPEECH_FAILURE_EXTRACT_FINISH:
    return "h2_gizclaw_speech_extract_finish";
  case SPEECH_FAILURE_EXTRACT_VALIDATE:
    return "speech_extract_validate";
  }
  return "speech_unknown";
}

static int collect_extraction(void *user, h2_gizclaw_str_t transcript,
                              h2_gizclaw_str_t result_json) {
  extraction_result_t *result = user;
  if (result == NULL || transcript.data == NULL || result_json.data == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  char transcript_text[1024];
  char json_text[1024];
  if (transcript.len >= sizeof(transcript_text) ||
      result_json.len >= sizeof(json_text)) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memcpy(transcript_text, transcript.data, transcript.len);
  transcript_text[transcript.len] = '\0';
  memcpy(json_text, result_json.data, result_json.len);
  json_text[result_json.len] = '\0';
  result->transcript_has_marker = contains_blue(transcript_text);
  result->json_has_marker = contains_blue(json_text);
  return H2_PAL_OK;
}

static int run_speech(h2_gizclaw_e2e_fixture_t *fixture) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  const h2_gizclaw_speech_transcribe_options_t transcribe_options = {
      .model_name = h2_gizclaw_e2e_str("asr"),
      .content_type = h2_gizclaw_e2e_str("audio/L16;rate=16000;channels=1"),
      .language = h2_gizclaw_e2e_str("en"),
  };
  int rc = H2_PAL_ERR_IO;
  speech_failure_stage_t failure_stage = SPEECH_FAILURE_TRANSCRIBE_OPEN;
  char transcript[1024];
  size_t transcript_len = 0u;
  for (unsigned attempt = 0u; attempt < 5u; ++attempt) {
    h2_gizclaw_speech_upload_t *upload = NULL;
    rc = h2_gizclaw_client_speech_transcribe_open(client, &transcribe_options,
                                                  &upload);
    failure_stage = SPEECH_FAILURE_TRANSCRIBE_OPEN;
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_speech_transcribe_write(upload, fixture->pcm,
                                              fixture->pcm_len);
      failure_stage = SPEECH_FAILURE_TRANSCRIBE_WRITE;
    }
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_speech_transcribe_finish(
          upload, transcript, sizeof(transcript), &transcript_len);
      upload = NULL;
      failure_stage = SPEECH_FAILURE_TRANSCRIBE_FINISH;
    }
    h2_gizclaw_speech_transcribe_cancel(upload);
    if (rc == H2_PAL_OK && transcript_len > 0u && contains_blue(transcript))
      break;
    if (rc == H2_PAL_OK) {
      rc = H2_PAL_ERR_INVALID_STATE;
      failure_stage = SPEECH_FAILURE_TRANSCRIBE_VALIDATE;
    }
    if (attempt + 1u < 5u)
      (void)h2_pal_time_sleep_ms(fixture->time, 500u * (attempt + 1u));
  }
  if (rc != H2_PAL_OK)
    return checked(speech_failure_symbol(failure_stage), "speech", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_speech_transcribe_open", "speech",
                          H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_speech_transcribe_write", "speech",
                          H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_speech_transcribe_finish", "speech",
                          H2_PAL_OK);

  const h2_gizclaw_speech_extract_options_t extract_options = {
      .asr_model_name = h2_gizclaw_e2e_str("asr"),
      .extract_model_name =
          h2_gizclaw_e2e_str("user-chat-with-assistant.extract"),
      .content_type = h2_gizclaw_e2e_str("audio/L16;rate=16000;channels=1"),
      .language = h2_gizclaw_e2e_str("en"),
      .schema_json = h2_gizclaw_e2e_str(
          "{\"type\":\"object\",\"properties\":{\"color\":{\"type\":\"string\","
          "\"enum\":[\"blue\"]}},\"required\":[\"color\"],"
          "\"additionalProperties\":false}"),
      .instruction = h2_gizclaw_e2e_str("Extract the spoken color marker."),
  };
  extraction_result_t extraction = {0};
  for (unsigned attempt = 0u; attempt < 5u; ++attempt) {
    h2_gizclaw_speech_upload_t *upload = NULL;
    extraction = (extraction_result_t){0};
    rc = h2_gizclaw_client_speech_extract_open(client, &extract_options,
                                               &upload);
    failure_stage = SPEECH_FAILURE_EXTRACT_OPEN;
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_speech_extract_write(upload, fixture->pcm,
                                           fixture->pcm_len);
      failure_stage = SPEECH_FAILURE_EXTRACT_WRITE;
    }
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_speech_extract_finish(upload, collect_extraction,
                                            &extraction);
      upload = NULL;
      failure_stage = SPEECH_FAILURE_EXTRACT_FINISH;
    }
    h2_gizclaw_speech_extract_cancel(upload);
    if (rc == H2_PAL_OK && extraction.transcript_has_marker &&
        extraction.json_has_marker) {
      break;
    }
    if (rc == H2_PAL_OK) {
      rc = H2_PAL_ERR_INVALID_STATE;
      failure_stage = SPEECH_FAILURE_EXTRACT_VALIDATE;
    }
    if (attempt + 1u < 5u)
      (void)h2_pal_time_sleep_ms(fixture->time, 500u * (attempt + 1u));
  }
  if (rc != H2_PAL_OK)
    return checked(speech_failure_symbol(failure_stage), "speech", rc);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_speech_extract_open", "speech",
                          H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_speech_extract_write", "speech",
                          H2_PAL_OK);
  h2_gizclaw_e2e_evidence("h2_gizclaw_speech_extract_finish", "speech",
                          H2_PAL_OK);
  return H2_PAL_OK;
}

static int run_telemetry(h2_gizclaw_e2e_fixture_t *fixture) {
  uint64_t wall_ms = 0u;
  int rc = h2_pal_time_get_wall_ms(fixture->time, &wall_ms);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_gizclaw_telemetry_observation_t observations[] = {
      {
          .kind = H2_GIZCLAW_TELEMETRY_BATTERY,
          .value.battery =
              {
                  .has_percent = true,
                  .percent = 64.4,
                  .has_charging = true,
                  .charging = false,
              },
      },
      {
          .kind = H2_GIZCLAW_TELEMETRY_SYSTEM,
          .value.system =
              {
                  .has_uptime_seconds = true,
                  .uptime_seconds = 1.0,
                  .has_software_version = true,
                  .software_version = {.data = "issue-644", .len = 9u},
              },
      },
  };
  const h2_gizclaw_telemetry_frame_t frame = {
      .sequence = 644u,
      .observed_at_unix_ms = (int64_t)wall_ms,
      .observations = observations,
      .observation_count = sizeof(observations) / sizeof(observations[0]),
  };
  h2_gizclaw_config_t client_config;
  memset(&client_config, 0, sizeof(client_config));
  rc = h2_gizclaw_e2e_fixture_transfer_actor_to_service(
      fixture, H2_GIZCLAW_E2E_OWNER, &client_config);
  if (rc != H2_PAL_OK)
    return checked("h2_gizclaw_service_telemetry_send_async", "telemetry", rc);

  connectivity_state_t state;
  memset(&state, 0, sizeof(state));
  state.fixture = fixture;
  atomic_init(&state.complete, false);
  atomic_init(&state.result, H2_PAL_OK);
  const h2_gizclaw_service_config_t config = {
      .client_config = &client_config,
      .task = fixture->runtime->task,
      .queue = fixture->runtime->queue,
      .sync = fixture->runtime->sync,
      .net_task_options = {.min_stack_size = 32768u},
      .operation_capacity = 1u,
      .client_poll_timeout_ms = 10,
  };
  rc = h2_gizclaw_service_init(&config, &state.service);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_service_start(state.service);
  h2_gizclaw_telemetry_request_t *request = NULL;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_telemetry_send_async(
        state.service, 10u, &frame, connectivity_telemetry_complete, &state,
        &request);
  }
  if (rc == H2_PAL_OK)
    rc = connectivity_wait(&state);
  if (rc != H2_PAL_OK && request != NULL)
    (void)h2_gizclaw_telemetry_request_cancel(request);
  const int stop_rc = state.service == NULL
                          ? H2_PAL_OK
                          : h2_gizclaw_service_stop(state.service);
  if (state.service != NULL) {
    for (;;) {
      size_t dispatched = 0u;
      const int dispatch_rc =
          h2_gizclaw_service_dispatch(state.service, 8u, &dispatched);
      if (rc == H2_PAL_OK)
        rc = dispatch_rc;
      if (dispatch_rc != H2_PAL_OK || dispatched == 0u)
        break;
    }
  }
  const int deinit_rc = state.service == NULL
                            ? H2_PAL_OK
                            : h2_gizclaw_service_deinit(state.service);
  if (rc == H2_PAL_OK)
    rc = stop_rc;
  if (rc == H2_PAL_OK)
    rc = deinit_rc;
  return checked("h2_gizclaw_service_telemetry_send_async", "telemetry", rc);
}

int h2_gizclaw_e2e_run_rpc(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  enum rpc_domain_index {
    RPC_DOMAIN_PROFILE = 0,
    RPC_DOMAIN_CATALOG_WORKSPACE,
    RPC_DOMAIN_SPEECH,
    RPC_DOMAIN_WORKSPACE_RECONNECT,
    RPC_DOMAIN_CONTACT,
    RPC_DOMAIN_FRIEND,
    RPC_DOMAIN_GROUP,
    RPC_DOMAIN_GAMEPLAY,
    RPC_DOMAIN_PEER_NAME_ISOLATION,
    RPC_DOMAIN_TELEMETRY,
    RPC_DOMAIN_COUNT,
  };
  struct rpc_domain {
    const char *name;
    int (*run)(h2_gizclaw_e2e_fixture_t *fixture);
    uint16_t dependencies;
  };
  static const struct rpc_domain domains[] = {
      [RPC_DOMAIN_PROFILE] = {"profile", run_profile, 0u},
      [RPC_DOMAIN_CATALOG_WORKSPACE] = {"catalog-workspace",
                                        run_catalog_workspace, 0u},
      [RPC_DOMAIN_SPEECH] = {"speech", run_speech, 0u},
      [RPC_DOMAIN_WORKSPACE_RECONNECT] = {"workspace-reconnect",
                                          run_workspace_reconnect,
                                          1u << RPC_DOMAIN_CATALOG_WORKSPACE},
      [RPC_DOMAIN_CONTACT] = {"contact", run_contact, 0u},
      [RPC_DOMAIN_FRIEND] = {"friend", run_friend, 0u},
      [RPC_DOMAIN_GROUP] = {"group", run_group, 0u},
      [RPC_DOMAIN_GAMEPLAY] = {"gameplay", run_gameplay, 0u},
      [RPC_DOMAIN_PEER_NAME_ISOLATION] = {"peer-name-isolation",
                                          run_peer_name_isolation,
                                          (1u << RPC_DOMAIN_CATALOG_WORKSPACE) |
                                              (1u << RPC_DOMAIN_CONTACT) |
                                              (1u << RPC_DOMAIN_GROUP) |
                                              (1u << RPC_DOMAIN_GAMEPLAY)},
      [RPC_DOMAIN_TELEMETRY] = {"telemetry", run_telemetry, 0u},
  };
  _Static_assert(sizeof(domains) / sizeof(domains[0]) == RPC_DOMAIN_COUNT,
                 "RPC domain table and index must remain synchronized");
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  for (size_t index = 0u; index < RPC_DOMAIN_COUNT; ++index) {
    if (h2_gizclaw_e2e_report_select(&report, domains[index].name) !=
        H2_PAL_OK) {
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  int first_rc = H2_PAL_OK;
  for (size_t index = 0u; index < RPC_DOMAIN_COUNT; ++index) {
    h2_gizclaw_e2e_case_status_t status = H2_GIZCLAW_E2E_CASE_PASS;
    const char *blocked_by = NULL;
    int domain_rc = H2_PAL_OK;
    const bool cancelled =
        fixture->cancel_requested ||
        (fixture->config->should_stop != NULL &&
         fixture->config->should_stop(fixture->config->should_stop_user));
    if (cancelled) {
      status = H2_GIZCLAW_E2E_CASE_CANCELLED;
      domain_rc = H2_PAL_ERR_CLOSED;
    } else {
      for (size_t dependency = 0u; dependency < index; ++dependency) {
        if ((domains[index].dependencies & (1u << dependency)) != 0u &&
            report.cases[dependency].status != H2_GIZCLAW_E2E_CASE_PASS) {
          status = H2_GIZCLAW_E2E_CASE_BLOCKED;
          blocked_by = domains[dependency].name;
          domain_rc = report.cases[dependency].rc;
          break;
        }
      }
      if (status == H2_GIZCLAW_E2E_CASE_PASS) {
        domain_rc = domains[index].run(fixture);
        if (domain_rc != H2_PAL_OK) {
          status = H2_GIZCLAW_E2E_CASE_FAIL;
        }
      }
    }
    const int report_rc = h2_gizclaw_e2e_report_terminal(
        &report, domains[index].name, status, domain_rc, blocked_by);
    if (report_rc != H2_PAL_OK) {
      return report_rc;
    }
    printf("H2_GIZCLAW_E2E stage=rpc-domain case=%s status=%s rc=%d "
           "blocked_by=%s\n",
           domains[index].name, h2_gizclaw_e2e_case_status_name(status),
           domain_rc, blocked_by == NULL ? "-" : blocked_by);
    if (first_rc == H2_PAL_OK && status != H2_GIZCLAW_E2E_CASE_PASS) {
      first_rc = domain_rc;
    }
  }
  h2_gizclaw_e2e_report_cleanup(&report, H2_PAL_OK);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&report);
  printf("H2_GIZCLAW_E2E stage=rpc-summary selected=%zu pass=%zu fail=%zu "
         "error=%zu blocked=%zu cancelled=%zu terminal=%zu complete=%s\n",
         summary.selected, summary.pass, summary.fail, summary.error,
         summary.blocked, summary.cancelled, summary.terminal,
         summary.complete ? "true" : "false");
  if (!summary.complete) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return first_rc;
}

int h2_gizclaw_e2e_prepare_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = run_catalog_workspace(fixture);
  return rc;
}
