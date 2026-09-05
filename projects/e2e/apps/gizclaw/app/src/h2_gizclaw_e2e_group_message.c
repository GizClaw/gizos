#include "h2_gizclaw_e2e_group_message.h"

#include <string.h>

#define TIMEOUT 30000u
#define LIMIT 32u
#define MAX_PAGES 32u
#define CURSOR_BYTES 255u

static const char *const symbols[2][2] = {
    {"h2_gizclaw_resp_parse_friend_group_message_list",
     "h2_gizclaw_resp_parse_friend_group_message_get"},
    {"h2_gizclaw_rpc_friend_group_message_list",
     "h2_gizclaw_rpc_friend_group_message_get"}};
static const char *const proofs[2] = {"friend_group_message_list-assert",
                                      "friend_group_message_get-assert"};

union response {
  h2_gizclaw_friend_group_message_page_t page;
  h2_gizclaw_friend_group_message_t message;
};

static int record(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static bool owns(const h2_gizclaw_resp_storage_t *s, const void *p, size_t n) {
  uintptr_t base = (uintptr_t)s->data, ptr = (uintptr_t)p;
  return p && s->used <= s->capacity && ptr >= base &&
         ptr - base <= s->used && n <= s->used - (ptr - base);
}

static bool text(const h2_gizclaw_resp_storage_t *s, const char *p,
                 bool required, size_t max) {
  if (!p)
    return !required;
  if (!owns(s, p, 1u))
    return false;
  const char *end = memchr(p, '\0', s->used - ((uintptr_t)p - (uintptr_t)s->data));
  return end && (size_t)(end - p) <= max && (!required || end != p);
}

static bool equal(const char *actual, h2_gizclaw_str_t expected) {
  return strlen(actual) == expected.len &&
         memcmp(actual, expected.data, expected.len) == 0;
}

static bool valid_message(const h2_gizclaw_e2e_fixture_t *f,
                           const h2_gizclaw_resp_storage_t *s,
                           const h2_gizclaw_friend_group_message_t *m) {
  return text(s, m->history_id, true, H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES) &&
         text(s, m->friend_group_name, true, H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES) &&
         strcmp(m->friend_group_name, f->friend_group_name) == 0 &&
         text(s, m->sender_peer_public_key, false, sizeof(f->actors[0].public_key) - 1u) &&
         text(s, m->created_at, false, SIZE_MAX) &&
         text(s, m->expires_at, false, SIZE_MAX) &&
         text(s, m->name, false, SIZE_MAX) &&
         text(s, m->text, false, H2_GIZCLAW_WORKSPACE_HISTORY_TEXT_MAX_BYTES);
}

static bool valid_target(const h2_gizclaw_e2e_fixture_t *f,
                          const h2_gizclaw_friend_group_message_t *m,
                          h2_gizclaw_str_t history) {
  /* The producer uploads the owner's PCM. Unknown types on unrelated rows
   * remain legal, but they cannot stand in for this specific user message. */
  return equal(m->history_id, history) && m->audio_available &&
         m->type == H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_GEAR &&
         m->sender_peer_public_key &&
         strcmp(m->sender_peer_public_key, f->actors[0].public_key) == 0;
}

static int call(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                unsigned api, unsigned get, uint64_t *id,
                h2_gizclaw_str_t arg, union response *out) {
  s->used = 0u;
  memset(out, 0, sizeof(*out));
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = f->actors[0].service;
  h2_gizclaw_str_t group = h2_gizclaw_e2e_str(f->friend_group_name);
  const char *stage = api ? "group-message-rpc" : "group-message-req";
  int rc;
  if (api) {
    rc = get ? h2_gizclaw_rpc_friend_group_message_get(
                   service, group, arg, TIMEOUT, s, &out->message)
             : h2_gizclaw_rpc_friend_group_message_list(
                   service, group, arg, LIMIT, TIMEOUT, s, &out->page);
    return record(symbols[api][get], stage, rc);
  }
  h2_gizclaw_req_t *request = NULL;
  rc = get ? h2_gizclaw_req_create_friend_group_message_get(
                 service, ++*id, group, arg, TIMEOUT, &request)
           : h2_gizclaw_req_create_friend_group_message_list(
                 service, ++*id, group, arg, LIMIT, TIMEOUT, &request);
  record(get ? "h2_gizclaw_req_create_friend_group_message_get"
             : "h2_gizclaw_req_create_friend_group_message_list", stage, rc);
  if (rc == H2_PAL_OK)
    rc = record("h2_gizclaw_req_do", stage,
                h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK)
    rc = record("h2_gizclaw_req_wait", stage,
                h2_gizclaw_req_wait(request, TIMEOUT));
  if (rc == H2_PAL_OK) {
    rc = get ? h2_gizclaw_resp_parse_friend_group_message_get(request, s, &out->message)
             : h2_gizclaw_resp_parse_friend_group_message_list(request, s, &out->page);
    record(symbols[api][get], stage, rc);
  }
  if (request) {
    if (rc != H2_PAL_OK)
      (void)record("h2_gizclaw_req_cancel", "group-message-cleanup",
                   h2_gizclaw_req_cancel(request));
    h2_gizclaw_req_release(request);
  }
  return rc;
}

static int list(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                unsigned api, uint64_t *id, h2_gizclaw_str_t history) {
  char cursor[CURSOR_BYTES + 1u] = {0};
  bool found = false;
  for (unsigned index = 0u; index < MAX_PAGES; ++index) {
    union response response;
    int rc = call(f, s, api, 0u, id, h2_gizclaw_e2e_str(cursor), &response);
    if (rc != H2_PAL_OK)
      return rc;
    const h2_gizclaw_friend_group_message_page_t *page = &response.page;
    if (s->used > s->capacity || page->count > LIMIT ||
        (page->count && (!owns(s, page->items, page->count * sizeof(*page->items)) ||
                         (uintptr_t)page->items % _Alignof(h2_gizclaw_friend_group_message_t))) ||
        !text(s, page->next_cursor, page->has_next, CURSOR_BYTES))
      return H2_PAL_ERR_FORMAT;
    for (size_t i = 0u; i < page->count; ++i) {
      const h2_gizclaw_friend_group_message_t *m = &page->items[i];
      if (!valid_message(f, s, m))
        return H2_PAL_ERR_FORMAT;
      for (size_t j = 0u; j < i; ++j)
        if (strcmp(m->history_id, page->items[j].history_id) == 0)
          return H2_PAL_ERR_FORMAT;
      if (equal(m->history_id, history)) {
        if (found || !valid_target(f, m, history))
          return H2_PAL_ERR_FORMAT;
        found = true;
      }
    }
    if (!page->has_next)
      return found ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
    if (strcmp(cursor, page->next_cursor) == 0)
      return H2_PAL_ERR_FORMAT;
    memcpy(cursor, page->next_cursor, strlen(page->next_cursor) + 1u);
  }
  return H2_PAL_ERR_NO_SPACE;
}

int h2_gizclaw_e2e_run_group_message(h2_gizclaw_e2e_fixture_t *f,
                                     h2_gizclaw_resp_storage_t *s,
                                     h2_gizclaw_str_t history) {
  if (!f || !s || !s->data || !s->capacity || !f->actors[0].service ||
      !history.data || !history.len ||
      history.len > H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES ||
      memchr(history.data, '\0', history.len))
    return H2_PAL_ERR_INVALID_ARG;
  if (!f->friend_group_created || !f->friend_group_name[0] ||
      !memchr(f->friend_group_name, '\0', sizeof(f->friend_group_name)) ||
      !f->actors[0].public_key[0] ||
      !memchr(f->actors[0].public_key, '\0', sizeof(f->actors[0].public_key)))
    return H2_PAL_ERR_INVALID_STATE;
  int rc = H2_PAL_OK;
  uint64_t id = 500u;
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    rc = list(f, s, api, &id, history);
    record(symbols[api][0], proofs[0], rc);
    if (rc != H2_PAL_OK)
      break;
    union response response;
    rc = call(f, s, api, 1u, &id, history, &response);
    if (rc == H2_PAL_OK &&
        (!valid_message(f, s, &response.message) ||
         !valid_target(f, &response.message, history)))
      rc = H2_PAL_ERR_FORMAT;
    record(symbols[api][1], proofs[1], rc);
  }
  s->used = 0u;
  return rc;
}
