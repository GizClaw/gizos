#include "h2_gizclaw_e2e_workflow.h"

#include <string.h>

#define WORKFLOW_TIMEOUT_MS 30000u
#define WORKFLOW_LIMIT 32u

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

int h2_gizclaw_e2e_select_workflow_name(
    const h2_gizclaw_workflow_page_t *workflows, char *out_name,
    size_t out_name_capacity) {
  if (workflows == NULL || out_name == NULL || out_name_capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  out_name[0] = '\0';
  if (workflows->count > 0u && workflows->items == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const char *selected = NULL;
  for (size_t i = 0u; i < workflows->count; ++i) {
    const char *name = workflows->items[i].name;
    if (name != NULL && name[0] != '\0' &&
        (selected == NULL || strcmp(name, selected) < 0))
      selected = name;
  }
  if (selected == NULL)
    return H2_PAL_ERR_NOT_FOUND;
  const size_t len = strlen(selected);
  if (len >= out_name_capacity)
    return H2_PAL_ERR_TRUNCATED;
  memcpy(out_name, selected, len + 1u);
  return H2_PAL_OK;
}

static bool metadata_matches(const h2_gizclaw_e2e_fixture_t *fixture,
                             const char *profile, const char *revision) {
  return profile != NULL &&
         strcmp(profile, fixture->runtime_profile_name) == 0 &&
         revision != NULL && revision[0] != '\0';
}

static int validate_list(h2_gizclaw_e2e_fixture_t *fixture,
                         const h2_gizclaw_workflow_page_t *page, char *selected,
                         size_t capacity) {
  if (!metadata_matches(fixture, page->runtime_profile_name,
                        page->runtime_profile_revision) ||
      page->count == 0u || page->count > WORKFLOW_LIMIT ||
      page->items == NULL ||
      (page->has_next &&
       (page->next_cursor == NULL || page->next_cursor[0] == '\0')))
    return H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0u; i < page->count; ++i) {
    const h2_gizclaw_workflow_t *workflow = &page->items[i];
    if (workflow->name == NULL || workflow->name[0] == '\0' ||
        workflow->collection == NULL ||
        strcmp(workflow->collection, "assistants") != 0)
      return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_gizclaw_e2e_select_workflow_name(page, selected, capacity);
}

static int submit_wait(h2_gizclaw_req_t *request) {
  int rc = checked("h2_gizclaw_req_do", "workflow-req",
                   h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_req_wait", "workflow-req",
                 h2_gizclaw_req_wait(request, WORKFLOW_TIMEOUT_MS));
  return rc;
}

static void release_request(h2_gizclaw_req_t *request, int rc) {
  if (request != NULL && rc != H2_PAL_OK)
    (void)checked("h2_gizclaw_req_cancel", "workflow-cleanup",
                  h2_gizclaw_req_cancel(request));
  h2_gizclaw_req_release(request);
}

int h2_gizclaw_e2e_run_workflow(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_gizclaw_resp_storage_t *storage) {
  if (fixture == NULL || storage == NULL || storage->data == NULL ||
      storage->capacity == 0u ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL ||
      fixture->runtime_profile_name[0] == '\0' ||
      memchr(fixture->runtime_profile_name, '\0',
             sizeof(fixture->runtime_profile_name)) == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  fixture->workflow_name[0] = '\0';
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  char selected[H2_GIZCLAW_E2E_NAME_CAPACITY] = {0};
  int rc = H2_PAL_OK;
  // Both API families select a server-provided alias, never a hard-coded model.
  for (unsigned req_api = 0u; req_api < 2u && rc == H2_PAL_OK; ++req_api) {
    storage->used = 0u;
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, WORKFLOW_TIMEOUT_MS))
      return H2_PAL_ERR_TIMEOUT;
    h2_gizclaw_workflow_page_t page = {0};
    h2_gizclaw_req_t *request = NULL;
    const char *list_symbol = req_api ? "h2_gizclaw_resp_parse_workflow_list"
                                      : "h2_gizclaw_rpc_workflow_list";
    if (req_api) {
      rc = checked("h2_gizclaw_req_create_workflow_list", "workflow-req",
                   h2_gizclaw_req_create_workflow_list(
                       service, 21u, h2_gizclaw_e2e_str("assistants"),
                       (h2_gizclaw_str_t){0}, WORKFLOW_LIMIT,
                       WORKFLOW_TIMEOUT_MS, &request));
      if (rc == H2_PAL_OK)
        rc = submit_wait(request);
      if (rc == H2_PAL_OK)
        rc = checked(
            list_symbol, "workflow-req",
            h2_gizclaw_resp_parse_workflow_list(request, storage, &page));
    } else {
      rc = checked(list_symbol, "workflow-rpc",
                   h2_gizclaw_rpc_workflow_list(
                       service, h2_gizclaw_e2e_str("assistants"),
                       (h2_gizclaw_str_t){0}, WORKFLOW_LIMIT,
                       WORKFLOW_TIMEOUT_MS, storage, &page));
    }
    release_request(request, rc);
    if (rc == H2_PAL_OK)
      rc = checked(list_symbol, "workflow-assert",
                   validate_list(fixture, &page, selected, sizeof(selected)));
    storage->used = 0u;
    if (rc != H2_PAL_OK)
      break;
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, WORKFLOW_TIMEOUT_MS))
      return H2_PAL_ERR_TIMEOUT;
    h2_gizclaw_workflow_get_result_t result = {0};
    request = NULL;
    const char *get_symbol = req_api ? "h2_gizclaw_resp_parse_workflow_get"
                                     : "h2_gizclaw_rpc_workflow_get";
    if (req_api) {
      rc = checked("h2_gizclaw_req_create_workflow_get", "workflow-req",
                   h2_gizclaw_req_create_workflow_get(
                       service, 22u, h2_gizclaw_e2e_str(selected),
                       WORKFLOW_TIMEOUT_MS, &request));
      if (rc == H2_PAL_OK)
        rc = submit_wait(request);
      if (rc == H2_PAL_OK)
        rc = checked(
            get_symbol, "workflow-req",
            h2_gizclaw_resp_parse_workflow_get(request, storage, &result));
    } else {
      rc = checked(
          get_symbol, "workflow-rpc",
          h2_gizclaw_rpc_workflow_get(service, h2_gizclaw_e2e_str(selected),
                                      WORKFLOW_TIMEOUT_MS, storage, &result));
    }
    release_request(request, rc);
    if (rc == H2_PAL_OK) {
      const bool matches =
          metadata_matches(fixture, result.runtime_profile_name,
                           result.runtime_profile_revision) &&
          result.workflow.name != NULL &&
          strcmp(result.workflow.name, selected) == 0 &&
          result.workflow.collection != NULL &&
          strcmp(result.workflow.collection, "assistants") == 0;
      rc = checked(get_symbol, "workflow-assert",
                   matches ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE);
    }
    storage->used = 0u;
  }
  if (rc == H2_PAL_OK)
    memcpy(fixture->workflow_name, selected, strlen(selected) + 1u);
  return rc;
}
