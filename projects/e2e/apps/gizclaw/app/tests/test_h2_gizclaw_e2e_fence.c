#include "h2_gizclaw_e2e_fence.h"
#include "h2_app_test_mem.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

struct h2_gizclaw_req {
  char level[65];
};

static struct {
  unsigned list, workflow, create, set, get, reload, invalid, releases;
  unsigned budget, fail_budget, mismatch_get;
  bool missing_option, wrong_active, wrong_active_omitted;
  char stored[65];
} state;
static h2_app_test_mem_t memory;
static unsigned service_token;
static struct h2_gizclaw_req request;

static void check_service(h2_gizclaw_service_t *service) {
  assert(service == (h2_gizclaw_service_t *)&service_token);
}
static void check_name(h2_gizclaw_str_t name, const char *expected) {
  assert(name.data != NULL && name.len == strlen(expected) &&
         memcmp(name.data, expected, name.len) == 0);
}
void h2_gizclaw_e2e_evidence(const char *case_id, const char *stage,
                              h2_pal_result_t rc) {
  assert(!strcmp(case_id, "workspace-fence") && stage != NULL);
  (void)rc;
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                      uint32_t timeout_ms) {
  assert(fixture != NULL && timeout_ms == 30000u);
  return ++state.budget != state.fail_budget;
}
h2_pal_result_t h2_gizclaw_rpc_workflow_list(
    h2_gizclaw_service_t *service, const h2_gizclaw_str_t *tags,
    size_t tag_count, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workflow_page_t *out) {
  check_service(service);
  assert(tags == NULL && !tag_count && !cursor.len && limit == 1u &&
         timeout_ms == 30000u && storage != NULL && out != NULL);
  static h2_gizclaw_safety_fence_option_t options[2] = {
      {.name = "safe"}, {.name = "guardian"}};
  ++state.list;
  out->runtime_profile_name = "e2e-profile";
  out->runtime_profile_revision = "rev-1";
  out->safety_fences = options;
  out->safety_fence_count = state.missing_option ? 1u : 2u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workflow_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workflow_get_result_t *out) {
  check_service(service);
  check_name(name, "fence-flow");
  assert(timeout_ms == 30000u && storage != NULL && out != NULL);
  ++state.workflow;
  out->workflow.name = "fence-flow";
  out->runtime_profile_revision = "rev-1";
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workflow,
    h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  check_service(service);
  check_name(workflow, "fence-flow");
  check_name(name, "test-workspace");
  assert(timeout_ms == 30000u && storage != NULL && out != NULL);
  ++state.create;
  out->name = "test-workspace";
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_set_parameters(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    const h2_gizclaw_workspace_parameters_patch_t *patch,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_t *out) {
  check_service(service);
  check_name(name, "test-workspace");
  assert(patch != NULL && timeout_ms == 30000u && storage != NULL && out != NULL);
  ++state.set;
  if (patch->has_safety_fence_level)
    strcpy(state.stored, patch->safety_fence_level);
  out->name = "test-workspace";
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_workspace_set_parameters(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    const h2_gizclaw_workspace_parameters_patch_t *patch,
    uint32_t timeout_ms, h2_gizclaw_req_t **out) {
  check_service(service);
  check_name(name, "test-workspace");
  assert((identity == 1u || identity == 2u) && patch != NULL &&
         timeout_ms == 30000u && out != NULL);
  *out = NULL;
  if (!strcmp(patch->safety_fence_level, "Invalid")) {
    ++state.invalid;
    return H2_PAL_ERR_INVALID_ARG;
  }
  assert(patch->has_safety_fence_level);
  strcpy(request.level, patch->safety_fence_level);
  *out = &request;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *req, void *user,
                                   h2_gizclaw_req_input_read_fn input_read,
                                   h2_gizclaw_req_output_write_fn output_write,
                                   h2_gizclaw_req_complete_fn on_complete) {
  assert(req == &request && user == NULL && input_read == NULL &&
         output_write == NULL && on_complete == NULL);
  strcpy(state.stored, request.level);
  ++state.set;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *req, uint32_t timeout) {
  assert(req == &request && timeout == H2_PAL_SYNC_WAIT_FOREVER);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *req) {
  assert(req == &request);
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *req) {
  assert(req == &request || req == NULL);
  if (req != NULL)
    ++state.releases;
}
h2_pal_result_t h2_gizclaw_resp_parse_workspace_set_parameters(
    const h2_gizclaw_req_t *req, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_t *out) {
  assert(req == &request && storage != NULL && out != NULL);
  out->name = "test-workspace";
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_get_result_t *out) {
  check_service(service);
  check_name(name, "test-workspace");
  assert(timeout_ms == 30000u && storage != NULL && out != NULL);
  ++state.get;
  out->workspace.name = "test-workspace";
  out->runtime_profile_revision = "rev-1";
  out->has_safety_fence_level = true;
  strcpy(out->safety_fence_level,
         state.mismatch_get == state.get ? "wrong" : state.stored);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_reload_with_options(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    const h2_gizclaw_workspace_parameters_patch_t *patch,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out) {
  check_service(service);
  check_name(name, "test-workspace");
  assert(timeout_ms == 30000u && storage != NULL && out != NULL);
  ++state.reload;
  if (patch != NULL) {
    assert(patch->has_safety_fence_level);
    strcpy(state.stored, patch->safety_fence_level);
  }
  out->runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING;
  out->active_workspace_name =
      state.wrong_active || (patch == NULL && state.wrong_active_omitted)
          ? "other" : "test-workspace";
  return H2_PAL_OK;
}
static int run(void) {
  h2_gizclaw_e2e_config_t config = {
      .fence_workflow_name = "fence-flow",
      .fence_first_id = "safe",
      .fence_second_id = "guardian"};
  h2_gizclaw_e2e_fixture_t fixture = {
      .config = &config,
      .allocator = &memory.api,
      .runtime_profile_name = "e2e-profile",
      .workspace_name = "test-workspace"};
  fixture.actors[H2_GIZCLAW_E2E_OWNER].service =
      (h2_gizclaw_service_t *)&service_token;
  int rc = h2_gizclaw_e2e_run_fence(&fixture);
  assert(fixture.workspace_created == (state.create != 0u));
  assert(memory.live_blocks == 0u);
  return rc;
}
int main(void) {
  h2_app_test_mem_init(&memory, NULL);
  assert(run() == H2_PAL_OK);
  assert(state.list == 1u && state.workflow == 1u && state.create == 1u);
  assert(state.set == 3u && state.get == 7u && state.reload == 3u &&
         state.invalid == 1u && state.releases == 1u);

  memset(&state, 0, sizeof(state));
  state.missing_option = true;
  assert(run() == H2_PAL_ERR_INVALID_STATE);
  assert(state.create == 0u && state.get == 0u);

  memset(&state, 0, sizeof(state));
  state.wrong_active = true;
  assert(run() == H2_PAL_ERR_INVALID_STATE);
  assert(state.create == 1u && state.reload == 1u && state.get == 1u);

  /* The omitted-fence reload must also leave this Workspace active; a stored
   * value read back from it proves nothing about another active one. */
  memset(&state, 0, sizeof(state));
  state.wrong_active_omitted = true;
  assert(run() == H2_PAL_ERR_INVALID_STATE);
  assert(state.reload == 3u && state.get == 5u && state.invalid == 0u);

  memset(&state, 0, sizeof(state));
  state.mismatch_get = 1u;
  assert(run() == H2_PAL_ERR_INVALID_STATE);
  assert(state.create == 1u && state.reload == 0u && state.get == 1u);

  memset(&state, 0, sizeof(state));
  state.fail_budget = 1u;
  assert(run() == H2_PAL_ERR_TIMEOUT);
  assert(state.create == 1u && state.set == 0u && state.get == 0u);
  return 0;
}
