#include "h2_gizclaw_e2e_fence.h"

#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#include <string.h>

#define FENCE_TIMEOUT_MS 30000u
#define FENCE_STORAGE_BYTES (16u * 1024u)

static h2_gizclaw_str_t span(const char *value) {
  return (h2_gizclaw_str_t){value, strlen(value)};
}

static int evidence(const char *stage, h2_pal_result_t result) {
  h2_gizclaw_e2e_evidence("workspace-fence", stage, result);
  return result;
}

static bool configured(const h2_gizclaw_e2e_config_t *config) {
  return config != NULL && config->fence_workflow_name != NULL &&
         config->fence_workflow_name[0] != '\0' &&
         strlen(config->fence_workflow_name) <=
             H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES &&
         config->fence_first_id != NULL && config->fence_second_id != NULL &&
         config->fence_first_id[0] != '\0' &&
         config->fence_second_id[0] != '\0' &&
         strlen(config->fence_first_id) <=
             H2_GIZCLAW_SAFETY_FENCE_LEVEL_MAX_BYTES &&
         strlen(config->fence_second_id) <=
             H2_GIZCLAW_SAFETY_FENCE_LEVEL_MAX_BYTES &&
         strcmp(config->fence_first_id, config->fence_second_id) != 0;
}

static bool option_present(const h2_gizclaw_workflow_page_t *page,
                           const char *id) {
  for (size_t i = 0u; i < page->safety_fence_count; ++i)
    if (strcmp(page->safety_fences[i].name, id) == 0)
      return true;
  return false;
}

/* A reload proves a selection only when it left this Workspace RUNNING. */
static bool activated(const h2_gizclaw_workspace_activation_t *activation,
                      const char *workspace) {
  return activation->runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
         activation->active_workspace_name != NULL &&
         strcmp(activation->active_workspace_name, workspace) == 0;
}

static h2_pal_result_t readback(h2_gizclaw_service_t *service,
                                 h2_gizclaw_resp_storage_t *storage,
                                 const char *workspace, const char *revision,
                                 const char *expected_id) {
  storage->used = 0u;
  h2_gizclaw_workspace_get_result_t result = {0};
  h2_pal_result_t rc = h2_gizclaw_rpc_workspace_get(
      service, span(workspace), FENCE_TIMEOUT_MS, storage, &result);
  if (rc != H2_PAL_OK)
    return rc;
  return result.workspace.name != NULL &&
                 strcmp(result.workspace.name, workspace) == 0 &&
                 result.runtime_profile_revision != NULL &&
                 strcmp(result.runtime_profile_revision, revision) == 0 &&
                 result.has_safety_fence_level &&
                 strcmp(result.safety_fence_level, expected_id) == 0
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t set_fence(h2_gizclaw_service_t *service,
                                  h2_gizclaw_resp_storage_t *storage,
                                  const char *workspace, const char *id,
                                  bool request_api) {
  h2_gizclaw_workspace_parameters_patch_t patch = {
      .has_safety_fence_level = true};
  memcpy(patch.safety_fence_level, id, strlen(id) + 1u);
  storage->used = 0u;
  h2_gizclaw_workspace_t updated = {0};
  if (!request_api)
    return h2_gizclaw_rpc_workspace_set_parameters(
        service, span(workspace), &patch, FENCE_TIMEOUT_MS, storage, &updated);
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_set_parameters(
      service, 1u, span(workspace), &patch, FENCE_TIMEOUT_MS, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_set_parameters(request, storage,
                                                          &updated);
  if (rc != H2_PAL_OK && request != NULL)
    (void)h2_gizclaw_req_cancel(request);
  h2_gizclaw_req_release(request);
  return rc == H2_PAL_OK && updated.name != NULL &&
                 strcmp(updated.name, workspace) == 0
             ? H2_PAL_OK : rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
}

int h2_gizclaw_e2e_run_fence(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || !configured(fixture->config) ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL ||
      fixture->allocator == NULL)
    return evidence("preflight", H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_service_t *service =
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  const h2_gizclaw_e2e_config_t *config = fixture->config;
  uint8_t *bytes = h2_pal_mem_alloc(fixture->allocator, FENCE_STORAGE_BYTES);
  if (bytes == NULL)
    return evidence("storage", H2_PAL_ERR_NO_MEMORY);
  h2_gizclaw_resp_storage_t storage = {bytes, FENCE_STORAGE_BYTES, 0u};
  h2_gizclaw_workflow_page_t page = {0};
  h2_pal_result_t rc = h2_gizclaw_rpc_workflow_list(
      service, NULL, 0u, (h2_gizclaw_str_t){0}, 1u, FENCE_TIMEOUT_MS,
      &storage, &page);
  char revision[256] = {0};
  if (rc == H2_PAL_OK) {
    if (page.runtime_profile_name == NULL ||
        strcmp(page.runtime_profile_name, fixture->runtime_profile_name) != 0 ||
        page.runtime_profile_revision == NULL ||
        strlen(page.runtime_profile_revision) >= sizeof(revision) ||
        page.safety_fences == NULL ||
        !option_present(&page, config->fence_first_id) ||
        !option_present(&page, config->fence_second_id))
      rc = H2_PAL_ERR_INVALID_STATE;
    else
      strcpy(revision, page.runtime_profile_revision);
  }
  if (evidence("discover", rc) != H2_PAL_OK)
    goto done;
  storage.used = 0u;
  h2_gizclaw_workflow_get_result_t workflow = {0};
  rc = h2_gizclaw_rpc_workflow_get(
      service, span(config->fence_workflow_name), FENCE_TIMEOUT_MS,
      &storage, &workflow);
  if (rc == H2_PAL_OK &&
      (workflow.workflow.name == NULL ||
       strcmp(workflow.workflow.name, config->fence_workflow_name) != 0 ||
       workflow.runtime_profile_revision == NULL ||
       strcmp(workflow.runtime_profile_revision, revision) != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (evidence("workflow", rc) != H2_PAL_OK)
    goto done;

  fixture->workspace_created = true;
  fixture->workspace_actor_role = H2_GIZCLAW_E2E_OWNER;
  storage.used = 0u;
  h2_gizclaw_workspace_t created = {0};
  rc = h2_gizclaw_rpc_workspace_create(
      service, span(config->fence_workflow_name),
      span(fixture->workspace_name), FENCE_TIMEOUT_MS, &storage, &created);
  if (rc == H2_PAL_OK &&
      (created.name == NULL ||
       strcmp(created.name, fixture->workspace_name) != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (evidence("create", rc) != H2_PAL_OK)
    goto done;

  const char *levels[] = {config->fence_first_id,
                          config->fence_second_id};
  for (size_t i = 0u; i < 2u && rc == H2_PAL_OK; ++i) {
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, FENCE_TIMEOUT_MS)) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    rc = set_fence(service, &storage, fixture->workspace_name,
                   levels[i], i == 0u);
    if (evidence("set", rc) != H2_PAL_OK)
      break;
    rc = readback(service, &storage, fixture->workspace_name, revision,
                   levels[i]);
    if (evidence("stored", rc) != H2_PAL_OK)
      break;
    storage.used = 0u;
    h2_gizclaw_workspace_activation_t activation = {0};
    h2_gizclaw_workspace_parameters_patch_t patch = {
        .has_safety_fence_level = true};
    strcpy(patch.safety_fence_level, levels[i]);
    rc = h2_gizclaw_rpc_workspace_reload_with_options(
        service, span(fixture->workspace_name), &patch, FENCE_TIMEOUT_MS,
        &storage, &activation);
    if (rc == H2_PAL_OK && !activated(&activation, fixture->workspace_name))
      rc = H2_PAL_ERR_INVALID_STATE;
    if (evidence("reload", rc) != H2_PAL_OK)
      break;
    rc = readback(service, &storage, fixture->workspace_name, revision,
                   levels[i]);
    evidence("applied-readback", rc);
  }
  if (rc != H2_PAL_OK)
    goto done;

  h2_gizclaw_workspace_parameters_patch_t omitted = {
      .has_input = true, .input = H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK};
  storage.used = 0u;
  h2_gizclaw_workspace_t updated = {0};
  rc = h2_gizclaw_rpc_workspace_set_parameters(
      service, span(fixture->workspace_name), &omitted,
      FENCE_TIMEOUT_MS, &storage, &updated);
  if (rc == H2_PAL_OK)
    rc = readback(service, &storage, fixture->workspace_name, revision,
                   config->fence_second_id);
  if (evidence("omitted-preserves-stored", rc) != H2_PAL_OK)
    goto done;
  storage.used = 0u;
  h2_gizclaw_workspace_activation_t activation = {0};
  rc = h2_gizclaw_rpc_workspace_reload_with_options(
      service, span(fixture->workspace_name), NULL, FENCE_TIMEOUT_MS,
      &storage, &activation);
  if (rc == H2_PAL_OK && !activated(&activation, fixture->workspace_name))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = readback(service, &storage, fixture->workspace_name, revision,
                   config->fence_second_id);
  if (evidence("omitted-reload", rc) != H2_PAL_OK)
    goto done;

  h2_gizclaw_workspace_parameters_patch_t invalid = {
      .has_safety_fence_level = true, .safety_fence_level = "Invalid"};
  h2_gizclaw_req_t *request = NULL;
  rc = h2_gizclaw_req_create_workspace_set_parameters(
      service, 2u, span(fixture->workspace_name), &invalid,
      FENCE_TIMEOUT_MS, &request);
  if (rc == H2_PAL_ERR_INVALID_ARG && request == NULL)
    rc = H2_PAL_OK;
  else {
    h2_gizclaw_req_release(request);
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (evidence("invalid-local", rc) != H2_PAL_OK)
    goto done;
  rc = readback(service, &storage, fixture->workspace_name, revision,
                 config->fence_second_id);
  evidence("invalid-unchanged", rc);

done:
  h2_pal_mem_free(fixture->allocator, bytes);
  return rc;
}
