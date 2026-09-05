#include "h2_gizclaw_e2e_workspace.h"
#include <string.h>

#define TIMEOUT_MS 30000u
#define LIMIT 32u
#define MAX_PAGES 32u
enum method { LIST, GET, CREATE, INPUT, DELETE, ACTIVATE, HISTORY, RELOAD };
static const char *const creates[] = {
    "h2_gizclaw_req_create_workspace_list",
    "h2_gizclaw_req_create_workspace_get",
    "h2_gizclaw_req_create_workspace_create",
    "h2_gizclaw_req_create_workspace_set_input",
    "h2_gizclaw_req_create_workspace_delete",
    "h2_gizclaw_req_create_workspace_activate",
    "h2_gizclaw_req_create_workspace_history_list",
    "h2_gizclaw_req_create_workspace_reload"};
static const char *const parses[] = {
    "h2_gizclaw_resp_parse_workspace_list",
    "h2_gizclaw_resp_parse_workspace_get",
    "h2_gizclaw_resp_parse_workspace_create",
    "h2_gizclaw_resp_parse_workspace_set_input",
    "h2_gizclaw_resp_parse_workspace_delete",
    "h2_gizclaw_resp_parse_workspace_activate",
    "h2_gizclaw_resp_parse_workspace_history_list",
    "h2_gizclaw_resp_parse_workspace_reload"};
static const char *const rpcs[] = {"h2_gizclaw_rpc_workspace_list",
                                   "h2_gizclaw_rpc_workspace_get",
                                   "h2_gizclaw_rpc_workspace_create",
                                   "h2_gizclaw_rpc_workspace_set_input",
                                   "h2_gizclaw_rpc_workspace_delete",
                                   "h2_gizclaw_rpc_workspace_activate",
                                   "h2_gizclaw_rpc_workspace_history_list",
                                   "h2_gizclaw_rpc_workspace_reload"};
static const char *const proofs[] = {
    "workspace_list-assert",         "workspace_get-assert",
    "workspace_create-assert",       "workspace_set_input-assert",
    "workspace_delete-assert",       "workspace_activate-assert",
    "workspace_history_list-assert", "workspace_reload-assert"};
union response {
  h2_gizclaw_workspace_t object;
  h2_gizclaw_workspace_get_result_t get;
  h2_gizclaw_workspace_page_t page;
  h2_gizclaw_workspace_activation_t activation;
  h2_gizclaw_workspace_history_page_t history;
};
static int evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
static int proof(bool req, enum method method, int rc) {
  return evidence(req ? parses[method] : rpcs[method], proofs[method], rc);
}
static bool owned(const h2_gizclaw_resp_storage_t *s, const void *p, size_t n) {
  uintptr_t base = (uintptr_t)s->data, value = (uintptr_t)p;
  return p != NULL && s->used <= s->capacity && value >= base &&
         value - base <= s->used && n <= s->used - (value - base);
}
static bool text_valid(const h2_gizclaw_resp_storage_t *s, const char *p,
                       bool required, size_t max) {
  if (p == NULL)
    return !required;
  if (!owned(s, p, 1u))
    return false;
  size_t left = s->used - ((uintptr_t)p - (uintptr_t)s->data);
  const char *end = memchr(p, '\0', left);
  return end != NULL && (size_t)(end - p) <= max && (!required || p[0] != '\0');
}
static bool metadata(const h2_gizclaw_e2e_fixture_t *f,
                     const h2_gizclaw_resp_storage_t *s, const char *profile,
                     const char *revision) {
  return text_valid(s, profile, true, 255u) &&
         strcmp(profile, f->runtime_profile_name) == 0 &&
         text_valid(s, revision, true, 255u);
}
static bool object_valid(const h2_gizclaw_resp_storage_t *s,
                         const h2_gizclaw_workspace_t *w) {
  return text_valid(s, w->name, true, 255u) &&
         text_valid(s, w->workflow_name, true, 63u) &&
         text_valid(s, w->collection, false, 255u);
}
static bool matches(const h2_gizclaw_e2e_fixture_t *f,
                    const h2_gizclaw_resp_storage_t *s,
                    const h2_gizclaw_workspace_t *w) {
  return object_valid(s, w) && !w->system &&
         strcmp(w->name, f->workspace_name) == 0 &&
         strcmp(w->workflow_name, f->workflow_name) == 0;
}
static int call(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                h2_gizclaw_e2e_actor_role_t role, bool req, enum method method,
                uint64_t *identity, const char *next, union response *out) {
  s->used = 0u;
  memset(out, 0, sizeof(*out));
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = f->actors[role].service;
  h2_gizclaw_str_t name = h2_gizclaw_e2e_str(f->workspace_name);
  h2_gizclaw_str_t workflow = h2_gizclaw_e2e_str(f->workflow_name);
  h2_gizclaw_str_t collection = h2_gizclaw_e2e_str("assistants");
  h2_gizclaw_str_t cursor = h2_gizclaw_e2e_str(next);
  int rc = H2_PAL_ERR_INVALID_ARG;
  if (req) {
    h2_gizclaw_req_t *request = NULL;
    uint64_t id = (*identity)++;
    switch (method) {
    case LIST:
      rc = h2_gizclaw_req_create_workspace_list(service, id, collection, cursor,
                                                LIMIT, TIMEOUT_MS, &request);
      break;
    case GET:
      rc = h2_gizclaw_req_create_workspace_get(service, id, name, TIMEOUT_MS,
                                               &request);
      break;
    case CREATE:
      rc = h2_gizclaw_req_create_workspace_create(
          service, id, collection, workflow, name, TIMEOUT_MS, &request);
      break;
    case INPUT:
      rc = h2_gizclaw_req_create_workspace_set_input(
          service, id, name, H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
          TIMEOUT_MS, &request);
      break;
    case DELETE:
      rc = h2_gizclaw_req_create_workspace_delete(service, id, name, TIMEOUT_MS,
                                                  &request);
      break;
    case ACTIVATE:
      rc = h2_gizclaw_req_create_workspace_activate(service, id, name,
                                                    TIMEOUT_MS, &request);
      break;
    case RELOAD:
      rc = h2_gizclaw_req_create_workspace_reload(service, id, TIMEOUT_MS,
                                                  &request);
      break;
    case HISTORY:
      rc = h2_gizclaw_req_create_workspace_history_list(
          service, id, name, cursor, LIMIT,
          H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC, TIMEOUT_MS, &request);
      break;
    }
    evidence(creates[method], "workspace-req", rc);
    if (rc == H2_PAL_OK) {
      if (method == CREATE) {
        f->workspace_delete_acknowledged = false;
        f->workspace_created = true;
        f->workspace_actor_role = role;
      }
      rc = evidence("h2_gizclaw_req_do", "workspace-req",
                    h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
    }
    if (rc == H2_PAL_OK)
      rc = evidence("h2_gizclaw_req_wait", "workspace-req",
                    h2_gizclaw_req_wait(request, TIMEOUT_MS));
    if (rc == H2_PAL_OK) {
      switch (method) {
      case LIST:
        rc = h2_gizclaw_resp_parse_workspace_list(request, s, &out->page);
        break;
      case GET:
        rc = h2_gizclaw_resp_parse_workspace_get(request, s, &out->get);
        break;
      case CREATE:
        rc = h2_gizclaw_resp_parse_workspace_create(request, s, &out->object);
        break;
      case INPUT:
        rc =
            h2_gizclaw_resp_parse_workspace_set_input(request, s, &out->object);
        break;
      case DELETE:
        rc = h2_gizclaw_resp_parse_workspace_delete(request, s, &out->object);
        break;
      case ACTIVATE:
        rc = h2_gizclaw_resp_parse_workspace_activate(request, s,
                                                      &out->activation);
        break;
      case RELOAD:
        rc = h2_gizclaw_resp_parse_workspace_reload(request, s,
                                                    &out->activation);
        break;
      case HISTORY:
        rc = h2_gizclaw_resp_parse_workspace_history_list(request, s,
                                                          &out->history);
        break;
      }
      evidence(parses[method], "workspace-req", rc);
    }
    if (rc != H2_PAL_OK && request != NULL)
      (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  } else {
    if (method == CREATE) {
      f->workspace_delete_acknowledged = false;
      f->workspace_created = true;
      f->workspace_actor_role = role;
    }
    switch (method) {
    case LIST:
      rc = h2_gizclaw_rpc_workspace_list(service, collection, cursor, LIMIT,
                                         TIMEOUT_MS, s, &out->page);
      break;
    case GET:
      rc =
          h2_gizclaw_rpc_workspace_get(service, name, TIMEOUT_MS, s, &out->get);
      break;
    case CREATE:
      rc = h2_gizclaw_rpc_workspace_create(service, collection, workflow, name,
                                           TIMEOUT_MS, s, &out->object);
      break;
    case INPUT:
      rc = h2_gizclaw_rpc_workspace_set_input(
          service, name, H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK, TIMEOUT_MS, s,
          &out->object);
      break;
    case DELETE:
      rc = h2_gizclaw_rpc_workspace_delete(service, name, TIMEOUT_MS, s,
                                           &out->object);
      break;
    case ACTIVATE:
      rc = h2_gizclaw_rpc_workspace_activate(service, name, TIMEOUT_MS, s,
                                             &out->activation);
      break;
    case RELOAD:
      rc = h2_gizclaw_rpc_workspace_reload(service, TIMEOUT_MS, s,
                                           &out->activation);
      break;
    case HISTORY:
      rc = h2_gizclaw_rpc_workspace_history_list(
          service, name, cursor, LIMIT, H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC,
          TIMEOUT_MS, s, &out->history);
      break;
    }
    evidence(rpcs[method], "workspace-rpc", rc);
  }
  if (rc == H2_PAL_OK && s->used > s->capacity)
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}
static int get(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
               h2_gizclaw_e2e_actor_role_t role, bool req, uint64_t *id,
               bool ready) {
  union response r;
  int rc = call(f, s, role, req, GET, id, "", &r);
  if (rc == H2_PAL_OK && (!matches(f, s, &r.get.workspace) ||
                          !metadata(f, s, r.get.runtime_profile_name,
                                    r.get.runtime_profile_revision) ||
                          (ready && !r.get.workspace.available)))
    rc = H2_PAL_ERR_INVALID_STATE;
  return proof(req, GET, rc);
}
static int list(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                h2_gizclaw_e2e_actor_role_t role, bool req, uint64_t *id,
                bool present) {
  char cursor[256] = {0};
  size_t found = 0u;
  for (unsigned page = 0; page < MAX_PAGES; ++page) {
    union response r;
    int rc = call(f, s, role, req, LIST, id, cursor, &r);
    h2_gizclaw_workspace_page_t *p = &r.page;
    if (rc == H2_PAL_OK &&
        (!metadata(f, s, p->runtime_profile_name,
                   p->runtime_profile_revision) ||
         p->count > LIMIT ||
         !text_valid(s, p->next_cursor, p->has_next, 255u) ||
         (p->count && ((uintptr_t)p->items % _Alignof(h2_gizclaw_workspace_t) ||
                       !owned(s, p->items, p->count * sizeof(*p->items))))))
      rc = H2_PAL_ERR_INVALID_STATE;
    for (size_t i = 0; rc == H2_PAL_OK && i < p->count; ++i) {
      if (!object_valid(s, &p->items[i])) {
        rc = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      for (size_t j = 0; j < i; ++j)
        if (strcmp(p->items[j].name, p->items[i].name) == 0)
          rc = H2_PAL_ERR_INVALID_STATE;
      if (strcmp(p->items[i].name, f->workspace_name) == 0) {
        if (!matches(f, s, &p->items[i]) || ++found > 1u)
          rc = H2_PAL_ERR_INVALID_STATE;
      }
    }
    if (rc != H2_PAL_OK)
      return proof(req, LIST, rc);
    if (!p->has_next)
      return proof(req, LIST,
                   found == (present ? 1u : 0u) ? H2_PAL_OK
                                                : H2_PAL_ERR_INVALID_STATE);
    if (strcmp(cursor, p->next_cursor) == 0)
      return proof(req, LIST, H2_PAL_ERR_INVALID_STATE);
    memcpy(cursor, p->next_cursor, strlen(p->next_cursor) + 1u);
  }
  return proof(req, LIST, H2_PAL_ERR_NO_SPACE);
}

static int history(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                   h2_gizclaw_e2e_actor_role_t role, bool req, uint64_t *id) {
  char cursor[256] = {0};
  for (unsigned page = 0; page < MAX_PAGES; ++page) {
    union response r;
    int rc = call(f, s, role, req, HISTORY, id, cursor, &r);
    h2_gizclaw_workspace_history_page_t *p = &r.history;
    if (rc == H2_PAL_OK &&
        (!p->available || p->count > LIMIT ||
         !text_valid(s, p->next_cursor, p->has_next, 255u) ||
         !text_valid(s, p->message, false, 4096u) ||
         (p->count && ((uintptr_t)p->items %
                           _Alignof(h2_gizclaw_workspace_history_entry_t) ||
                       !owned(s, p->items, p->count * sizeof(*p->items))))))
      rc = H2_PAL_ERR_INVALID_STATE;
    for (size_t i = 0; rc == H2_PAL_OK && i < p->count; ++i) {
      h2_gizclaw_workspace_history_entry_t *e = &p->items[i];
      if (!text_valid(s, e->id, true, 255u) ||
          !text_valid(s, e->name, false, 255u) ||
          !text_valid(s, e->text, false, 4096u) ||
          !text_valid(s, e->created_at, false, 255u) ||
          !text_valid(s, e->gear_id, false, 255u)) {
        rc = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      for (size_t j = 0; j < i; ++j)
        if (strcmp(p->items[j].id, e->id) == 0)
          rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc != H2_PAL_OK || !p->has_next)
      return proof(req, HISTORY, rc);
    if (strcmp(cursor, p->next_cursor) == 0)
      return proof(req, HISTORY, H2_PAL_ERR_INVALID_STATE);
    memcpy(cursor, p->next_cursor, strlen(p->next_cursor) + 1u);
  }
  return proof(req, HISTORY, H2_PAL_ERR_NO_SPACE);
}
static int create(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                  h2_gizclaw_e2e_actor_role_t role, bool req, uint64_t *id) {
  union response r;
  int rc = call(f, s, role, req, CREATE, id, "", &r);
  if (rc == H2_PAL_OK &&
      (!matches(f, s, &r.object) || r.object.collection == NULL ||
       strcmp(r.object.collection, "assistants") != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = get(f, s, role, req, id, false);
  if (rc == H2_PAL_OK)
    rc = list(f, s, role, req, id, true);
  return proof(req, CREATE, rc);
}
static int configure(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                     h2_gizclaw_e2e_actor_role_t role, bool req, uint64_t *id) {
  union response r;
  int rc = call(f, s, role, req, INPUT, id, "", &r);
  if (rc == H2_PAL_OK && (!matches(f, s, &r.object) || !r.object.available))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = get(f, s, role, req, id, true);
  proof(req, INPUT, rc);
  if (rc != H2_PAL_OK)
    return rc;
  rc = call(f, s, role, req, ACTIVATE, id, "", &r);
  if (rc == H2_PAL_OK &&
      (!text_valid(s, r.activation.workspace_name, true, 255u) ||
       strcmp(r.activation.workspace_name, f->workspace_name) != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  proof(req, ACTIVATE, rc);
  if (rc != H2_PAL_OK)
    return rc;
  rc = call(f, s, role, req, RELOAD, id, "", &r);
  if (rc == H2_PAL_OK) {
    h2_gizclaw_workspace_activation_t *a = &r.activation;
    if (!text_valid(s, a->workspace_name, true, 255u) ||
        !text_valid(s, a->active_workspace_name, true, 255u) ||
        !text_valid(s, a->pending_workspace_name, false, 255u) ||
        !text_valid(s, a->workflow_name, false, 63u) ||
        strcmp(a->workspace_name, f->workspace_name) != 0 ||
        strcmp(a->active_workspace_name, f->workspace_name) != 0 ||
        (a->workflow_name && a->workflow_name[0] &&
         strcmp(a->workflow_name, f->workflow_name) != 0) ||
        (a->pending_workspace_name && a->pending_workspace_name[0]) ||
        a->runtime_state != H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  proof(req, RELOAD, rc);
  return rc == H2_PAL_OK ? history(f, s, role, req, id) : rc;
}
static bool fixed_text(const char *value, size_t capacity) {
  return value[0] != '\0' && memchr(value, '\0', capacity) != NULL;
}
int h2_gizclaw_e2e_run_workspace(h2_gizclaw_e2e_fixture_t *f,
                                 h2_gizclaw_resp_storage_t *s, bool exercise) {
  if (f == NULL || s == NULL || s->data == NULL || !s->capacity ||
      f->actors[H2_GIZCLAW_E2E_OWNER].service == NULL ||
      !fixed_text(f->workspace_name, sizeof(f->workspace_name)) ||
      !fixed_text(f->workflow_name, sizeof(f->workflow_name)) ||
      !fixed_text(f->runtime_profile_name, sizeof(f->runtime_profile_name)))
    return H2_PAL_ERR_INVALID_ARG;
  if (exercise && (f->actors[H2_GIZCLAW_E2E_FRIEND].service == NULL ||
                   f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].service == NULL))
    return H2_PAL_ERR_INVALID_STATE;
  if (f->workspace_created)
    return H2_PAL_ERR_INVALID_STATE;
  char original[sizeof(f->workspace_name)];
  memcpy(original, f->workspace_name, sizeof(original));
  const size_t original_len = strlen(original);
  if (exercise && original_len > sizeof(original) - sizeof("-req"))
    return H2_PAL_ERR_TRUNCATED;
  uint64_t id = 100u;
  for (unsigned api = 0; exercise && api < 2u; ++api) {
    /* Keep the precise pending name in the heap fixture on every failure. */
    memcpy(f->workspace_name, original, original_len);
    memcpy(f->workspace_name + original_len, api ? "-rpc" : "-req",
           sizeof("-req"));
    bool req = api == 0u;
    const h2_gizclaw_e2e_actor_role_t role =
        api == 0u ? H2_GIZCLAW_E2E_FRIEND : H2_GIZCLAW_E2E_GROUP_MEMBER;
    int rc = create(f, s, role, req, &id);
    if (rc == H2_PAL_OK)
      rc = configure(f, s, role, req, &id);
    if (rc != H2_PAL_OK)
      return rc;
    union response r;
    rc = call(f, s, role, req, DELETE, &id, "", &r);
    if (rc == H2_PAL_OK && !matches(f, s, &r.object))
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) {
      f->workspace_delete_acknowledged = true;
    }
    proof(req, DELETE, rc);
    if (rc != H2_PAL_OK)
      return rc;
    f->workspace_created = false;
    f->workspace_delete_acknowledged = false;
    f->workspace_actor_role = H2_GIZCLAW_E2E_OWNER;
  }
  /* The normal fixture workspace remains available for reconnect and Voice. */
  memcpy(f->workspace_name, original, sizeof(original));
  int rc = create(f, s, H2_GIZCLAW_E2E_OWNER, false, &id);
  if (rc == H2_PAL_OK)
    rc = configure(f, s, H2_GIZCLAW_E2E_OWNER, false, &id);
  s->used = 0u;
  return rc;
}
