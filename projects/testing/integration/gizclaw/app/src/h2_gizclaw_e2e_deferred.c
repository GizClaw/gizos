#include "h2_gizclaw_e2e_rpc.h"

#include "h2_gizclaw_e2e_device_http.h"
#include "payload/management.pb.h"
#include "pb_encode.h"

#include <string.h>

enum deferred_phase {
  DEFERRED_IDLE,
  DEFERRED_PENDING,
  DEFERRED_READY,
  DEFERRED_CLOSED
};

typedef struct deferred_case {
  h2_gizclaw_resp_storage_t* storage;
  h2_atomic_int_t phase, calls, polls, closes, close_result;
  /* Published by IDLE, then immutable until close publishes CLOSED. */
  bool expire;
  int result;
  unsigned nested_calls;
  uint8_t bytes[4096];
  size_t size;
  char level[65];
} deferred_case_t;

static int deferred_poll(void* user,
                         h2_gizclaw_rpc_provider_response_t* response) {
  deferred_case_t* state = user;
  (void)h2_atomic_fetch_add(&state->polls, 1);
  if (h2_atomic_load(&state->phase) != DEFERRED_READY)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (state->result != H2_PAL_OK) return state->result;
  response->payload = (h2_gizclaw_rpc_bytes_t){state->bytes, state->size};
  return H2_PAL_OK;
}

static void deferred_close(void* user, int result) {
  deferred_case_t* state = user;
  h2_atomic_store(&state->close_result, result);
  (void)h2_atomic_fetch_add(&state->closes, 1);
  h2_atomic_store(&state->phase, DEFERRED_CLOSED);
}

static int deferred_invoke(void* user, h2_gizclaw_tool_t tool,
                           h2_gizclaw_rpc_bytes_t request,
                           h2_gizclaw_rpc_provider_response_t* response) {
  deferred_case_t* state = user;
  if (tool != H2_GIZCLAW_TOOL_AI_SAFETY_GET || request.len != 0u ||
      h2_atomic_load(&state->phase) != DEFERRED_IDLE)
    return H2_PAL_ERR_INVALID_STATE;
  (void)h2_atomic_fetch_add(&state->calls, 1);
  response->deferred = (h2_gizclaw_rpc_deferred_response_t){
      .user = state,
      .poll = deferred_poll,
      .close = deferred_close,
      .timeout_ms = state->expire ? 500u : 15000u,
  };
  h2_atomic_store(&state->phase, DEFERRED_PENDING);
  return H2_PAL_OK;
}

/* This runs on the App owner while HTTP waits on its independent job task.
 * Neither invoke nor poll performs a blocking request on the network owner. */
static void deferred_tick(h2_gizclaw_e2e_fixture_t* fixture) {
  deferred_case_t* state = fixture->product_state;
  if (h2_atomic_load(&state->phase) != DEFERRED_PENDING || state->expire ||
      h2_atomic_load(&state->polls) == 0)
    return;
  h2_gizclaw_service_t* service = fixture->actors[0].service;
  h2_gizclaw_resp_storage_t* storage = state->storage;
  storage->used = 0u;
  h2_gizclaw_workflow_page_t page = {0};
  int rc = h2_gizclaw_rpc_workflow_list(
      service, NULL, 0u, (h2_gizclaw_str_t){0}, 1u, 5000u, storage, &page);
  ++state->nested_calls;
  h2_gizclaw_e2e_evidence("deferred-management", "nested-discovery", rc);
  gizclaw_rpc_v1_ClientAISafetyGetResponse reply =
      gizclaw_rpc_v1_ClientAISafetyGetResponse_init_zero;
  if (rc == H2_PAL_OK &&
      (page.safety_fence_count == 0u || page.safety_fence_count > 16u ||
       page.safety_fences == NULL))
    rc = H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0u; rc == H2_PAL_OK && i < page.safety_fence_count; ++i) {
    const h2_gizclaw_safety_fence_option_t* source = &page.safety_fences[i];
    const char* display =
        source->has_display_name ? source->display_name : source->name;
    if (source->name[0] == '\0' ||
        strlen(source->name) >= sizeof(reply.state.options[i].id) ||
        strlen(display) >= sizeof(reply.state.options[i].display_name)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    strcpy(reply.state.options[i].id, source->name);
    strcpy(reply.state.options[i].display_name, display);
    ++reply.state.options_count;
  }
  storage->used = 0u;
  h2_gizclaw_workspace_get_result_t workspace = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_rpc_workspace_get(
        service, h2_gizclaw_e2e_str(fixture->workspace_name), 5000u, storage,
        &workspace);
    ++state->nested_calls;
    h2_gizclaw_e2e_evidence("deferred-management", "nested-readback", rc);
  }
  bool found = false;
  if (rc == H2_PAL_OK && workspace.has_safety_fence_level &&
      workspace.workspace.name != NULL &&
      strcmp(workspace.workspace.name, fixture->workspace_name) == 0) {
    for (size_t i = 0u; i < reply.state.options_count; ++i)
      if (strcmp(reply.state.options[i].id, workspace.safety_fence_level) == 0)
        found = true;
  }
  if (rc == H2_PAL_OK && !found) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    reply.has_state = true;
    strcpy(reply.state.level, workspace.safety_fence_level);
    strcpy(state->level, reply.state.level);
    pb_ostream_t stream =
        pb_ostream_from_buffer(state->bytes, sizeof(state->bytes));
    if (!pb_encode(&stream, gizclaw_rpc_v1_ClientAISafetyGetResponse_fields,
                   &reply))
      rc = H2_PAL_ERR_FORMAT;
    else
      state->size = stream.bytes_written;
  }
  state->result = rc;
  /* A timed-out call must never be resurrected by a late App completion. */
  int expected = DEFERRED_PENDING;
  (void)h2_atomic_compare_exchange_strong(&state->phase, &expected,
                                          DEFERRED_READY);
}

static void deferred_dispose(h2_gizclaw_e2e_fixture_t* fixture) {
  deferred_case_t* state = fixture->product_state;
  h2_atomic_destroy(&state->phase);
  h2_atomic_destroy(&state->calls);
  h2_atomic_destroy(&state->polls);
  h2_atomic_destroy(&state->closes);
  h2_atomic_destroy(&state->close_result);
  h2_pal_mem_free(fixture->allocator, state);
  fixture->product_state = NULL;
  fixture->product_dispose = NULL;
  fixture->app_tick = NULL;
  fixture->product_tool = (h2_gizclaw_tool_handler_t){0};
}

int h2_gizclaw_e2e_prepare_rpc(h2_gizclaw_e2e_fixture_t* fixture) {
  deferred_case_t* state = h2_pal_mem_alloc(fixture->allocator, sizeof(*state));
  if (state == NULL) return H2_PAL_ERR_NO_MEMORY;
  *state = (deferred_case_t){0};
  fixture->product_state = state;
  fixture->product_dispose = deferred_dispose;
  if (h2_atomic_init(&state->phase, DEFERRED_CLOSED) != H2_ATOMIC_OK ||
      h2_atomic_init(&state->calls, 0) != H2_ATOMIC_OK ||
      h2_atomic_init(&state->polls, 0) != H2_ATOMIC_OK ||
      h2_atomic_init(&state->closes, 0) != H2_ATOMIC_OK ||
      h2_atomic_init(&state->close_result, H2_PAL_ERR_INVALID_STATE) !=
          H2_ATOMIC_OK) {
    deferred_dispose(fixture);
    return H2_PAL_ERR_NO_MEMORY;
  }
  fixture->product_tool = (h2_gizclaw_tool_handler_t){
      H2_GIZCLAW_TOOL_AI_SAFETY_GET, deferred_invoke, state};
  return H2_PAL_OK;
}

static int deferred_round(h2_gizclaw_e2e_api_t* api, bool expire) {
  deferred_case_t* state = api->fixture->product_state;
  state->expire = expire;
  state->nested_calls = 0u;
  state->size = 0u;
  state->result = H2_PAL_ERR_INVALID_STATE;
  h2_atomic_store(&state->calls, 0);
  h2_atomic_store(&state->polls, 0);
  h2_atomic_store(&state->closes, 0);
  h2_atomic_store(&state->close_result, H2_PAL_ERR_INVALID_STATE);
  h2_atomic_store(&state->phase, DEFERRED_IDLE);
  int rc =
      h2_gizclaw_e2e_tool_call(api, "ai.safety.get", "{}", expire ? 504 : 200);
  if (rc == H2_PAL_OK && (h2_atomic_load(&state->phase) != DEFERRED_CLOSED ||
                          h2_atomic_load(&state->calls) != 1 ||
                          h2_atomic_load(&state->closes) != 1 ||
                          h2_atomic_load(&state->polls) == 0 ||
                          h2_atomic_load(&state->close_result) !=
                              (expire ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK) ||
                          state->nested_calls != (expire ? 0u : 2u)))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK &&
      !(expire ? h2_gizclaw_e2e_api_text_is(api, "error.code", "DEVICE_TIMEOUT")
               : h2_gizclaw_e2e_api_text_is(api, "result.state.level",
                                            state->level)))
    rc = H2_PAL_ERR_INVALID_STATE;
  h2_gizclaw_e2e_emit(
      "H2_GIZCLAW_E2E stage=deferred-management phase=%s calls=%d polls=%d "
      "closes=%d close_rc=%d nested=%u rc=%d\n",
      expire ? "timeout" : "success", h2_atomic_load(&state->calls),
      h2_atomic_load(&state->polls), h2_atomic_load(&state->closes),
      h2_atomic_load(&state->close_result), state->nested_calls, rc);
  return rc;
}

int h2_gizclaw_e2e_run_deferred(h2_gizclaw_e2e_fixture_t* fixture,
                                h2_gizclaw_resp_storage_t* storage) {
  if (fixture->product_state == NULL || !fixture->workspace_created ||
      fixture->workspace_actor_role != H2_GIZCLAW_E2E_OWNER ||
      fixture->config->device_api_url == NULL ||
      strncmp(fixture->config->device_api_url, "https://", 8u) != 0)
    return H2_PAL_ERR_INVALID_STATE;
  deferred_case_t* state = fixture->product_state;
  state->storage = storage;
  h2_gizclaw_e2e_api_t api = {.fixture = fixture};
  int rc = h2_yyjson_json_create(fixture->allocator, &api.provider);
  if (rc != H2_PAL_OK) return rc;
  api.json = h2_yyjson_json_api(api.provider);
  h2_gizclaw_service_t* service = fixture->actors[0].service;
  rc = h2_gizclaw_rpc_api_key_create(service,
                                     h2_gizclaw_e2e_str("gizos-deferred-e2e"),
                                     false, 15000u, &api.key);
  fixture->app_tick = deferred_tick;
  if (rc == H2_PAL_OK) rc = deferred_round(&api, false);
  if (rc == H2_PAL_OK) rc = deferred_round(&api, true);
  /* No reconnect: the same Service must recover after deadline cleanup. */
  if (rc == H2_PAL_OK) rc = deferred_round(&api, false);
  fixture->app_tick = NULL;
  state->storage = NULL;
  if (api.key.name[0] != '\0') {
    const int cleanup = h2_gizclaw_rpc_api_key_revoke(
        service, h2_gizclaw_e2e_str(api.key.name), 15000u);
    h2_gizclaw_e2e_evidence("deferred-management", "key-revoke", cleanup);
    if (rc == H2_PAL_OK) rc = cleanup;
  }
  memset(&api.key, 0, sizeof(api.key));
  (void)h2_pal_json_document_destroy(api.json, &api.document);
  (void)h2_yyjson_json_destroy(&api.provider);
  return rc;
}
