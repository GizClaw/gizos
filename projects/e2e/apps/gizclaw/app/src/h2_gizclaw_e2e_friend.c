#include "h2_gizclaw_e2e_friend.h"

#include <string.h>

#define FRIEND_TIMEOUT_MS 30000u
#define FRIEND_PAGE_LIMIT 32u
#define FRIEND_MAX_PAGES 32u

enum method { ADD, INFO, LIST, DELETE, TOKEN_CREATE, TOKEN_GET, TOKEN_CLEAR };
static const char *const rpc_symbols[] = {
    "h2_gizclaw_rpc_friend_add",
    "h2_gizclaw_rpc_friend_info_get",
    "h2_gizclaw_rpc_friend_list",
    "h2_gizclaw_rpc_friend_delete",
    "h2_gizclaw_rpc_friend_invite_token_create",
    "h2_gizclaw_rpc_friend_invite_token_get",
    "h2_gizclaw_rpc_friend_invite_token_clear"};
static const char *const parse_symbols[] = {
    "h2_gizclaw_resp_parse_friend_add",
    "h2_gizclaw_resp_parse_friend_info_get",
    "h2_gizclaw_resp_parse_friend_list",
    "h2_gizclaw_resp_parse_friend_delete",
    "h2_gizclaw_resp_parse_friend_invite_token_create",
    "h2_gizclaw_resp_parse_friend_invite_token_get",
    "h2_gizclaw_resp_parse_friend_invite_token_clear"};
static const char *const assertions[] = {"friend_add-assert",
                                         "friend_info_get-assert",
                                         "friend_list-assert",
                                         "friend_delete-assert",
                                         "friend_invite_token_create-assert",
                                         "friend_invite_token_get-assert",
                                         "friend_invite_token_clear-assert"};

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
static int proof(bool req_api, enum method method, int rc) {
  return checked(req_api ? parse_symbols[method] : rpc_symbols[method],
                 assertions[method], rc);
}
static bool owned(const h2_gizclaw_resp_storage_t *storage, const void *ptr,
                  size_t size) {
  const uintptr_t base = (uintptr_t)storage->data, value = (uintptr_t)ptr;
  return ptr != NULL && storage->used <= storage->capacity && value >= base &&
         value - base <= storage->used &&
         size <= storage->used - (value - base);
}
static bool text_valid(const h2_gizclaw_resp_storage_t *storage,
                       const char *value, bool required) {
  if (value == NULL)
    return !required;
  if (!owned(storage, value, 1u))
    return false;
  const size_t left =
      storage->used - ((uintptr_t)value - (uintptr_t)storage->data);
  return memchr(value, '\0', left) != NULL && (!required || value[0] != '\0');
}
static void record_obligation(h2_gizclaw_e2e_fixture_t *fixture,
                              enum method method) {
  if (method == TOKEN_CREATE)
    fixture->friend_invite_created = true;
  if (method == ADD)
    fixture->friendship_created = true;
}
static int call(h2_gizclaw_e2e_fixture_t *fixture,
                h2_gizclaw_resp_storage_t *storage, bool req_api,
                enum method method, uint64_t *identity, const char *argument,
                h2_gizclaw_friend_t *object, h2_gizclaw_friend_page_t *page,
                h2_gizclaw_invite_token_t *token) {
  storage->used = 0u;
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, FRIEND_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service =
      fixture
          ->actors[method >= TOKEN_CREATE ? H2_GIZCLAW_E2E_FRIEND
                                          : H2_GIZCLAW_E2E_OWNER]
          .service;
  const h2_gizclaw_str_t arg = h2_gizclaw_e2e_str(argument);
  int rc = H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  if (req_api) {
    const uint64_t id = (*identity)++;
    switch (method) {
    case ADD:
      rc = checked("h2_gizclaw_req_create_friend_add", "friend-req",
                   h2_gizclaw_req_create_friend_add(
                       service, id, arg, FRIEND_TIMEOUT_MS, &request));
      break;
    case INFO:
      rc = checked("h2_gizclaw_req_create_friend_info_get", "friend-req",
                   h2_gizclaw_req_create_friend_info_get(
                       service, id, arg, FRIEND_TIMEOUT_MS, &request));
      break;
    case LIST:
      rc = checked(
          "h2_gizclaw_req_create_friend_list", "friend-req",
          h2_gizclaw_req_create_friend_list(service, id, arg, FRIEND_PAGE_LIMIT,
                                            FRIEND_TIMEOUT_MS, &request));
      break;
    case DELETE:
      rc = checked("h2_gizclaw_req_create_friend_delete", "friend-req",
                   h2_gizclaw_req_create_friend_delete(
                       service, id, arg, FRIEND_TIMEOUT_MS, &request));
      break;
    case TOKEN_CREATE:
      rc = checked("h2_gizclaw_req_create_friend_invite_token_create",
                   "friend-req",
                   h2_gizclaw_req_create_friend_invite_token_create(
                       service, id, FRIEND_TIMEOUT_MS, &request));
      break;
    case TOKEN_GET:
      rc =
          checked("h2_gizclaw_req_create_friend_invite_token_get", "friend-req",
                  h2_gizclaw_req_create_friend_invite_token_get(
                      service, id, FRIEND_TIMEOUT_MS, &request));
      break;
    case TOKEN_CLEAR:
      rc = checked("h2_gizclaw_req_create_friend_invite_token_clear",
                   "friend-req",
                   h2_gizclaw_req_create_friend_invite_token_clear(
                       service, id, FRIEND_TIMEOUT_MS, &request));
      break;
    }
    if (rc == H2_PAL_OK) {
      record_obligation(fixture, method);
      rc = checked("h2_gizclaw_req_do", "friend-req",
                   h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
    }
    if (rc == H2_PAL_OK)
      rc = checked("h2_gizclaw_req_wait", "friend-req",
                   h2_gizclaw_req_wait(request, FRIEND_TIMEOUT_MS));
    if (rc == H2_PAL_OK) {
      switch (method) {
      case ADD:
        rc =
            checked(parse_symbols[method], "friend-req",
                    h2_gizclaw_resp_parse_friend_add(request, storage, object));
        break;
      case INFO:
        rc = checked(
            parse_symbols[method], "friend-req",
            h2_gizclaw_resp_parse_friend_info_get(request, storage, object));
        break;
      case LIST:
        rc = checked(parse_symbols[method], "friend-req",
                     h2_gizclaw_resp_parse_friend_list(request, storage, page));
        break;
      case DELETE:
        rc = checked(
            parse_symbols[method], "friend-req",
            h2_gizclaw_resp_parse_friend_delete(request, storage, object));
        break;
      case TOKEN_CREATE:
        rc = checked(parse_symbols[method], "friend-req",
                     h2_gizclaw_resp_parse_friend_invite_token_create(
                         request, storage, token));
        break;
      case TOKEN_GET:
        rc = checked(parse_symbols[method], "friend-req",
                     h2_gizclaw_resp_parse_friend_invite_token_get(
                         request, storage, token));
        break;
      case TOKEN_CLEAR:
        rc = checked(parse_symbols[method], "friend-req",
                     h2_gizclaw_resp_parse_friend_invite_token_clear(request));
        break;
      }
    }
    if (rc != H2_PAL_OK && request != NULL)
      (void)checked("h2_gizclaw_req_cancel", "friend-cleanup",
                    h2_gizclaw_req_cancel(request));
    h2_gizclaw_req_release(request);
  } else {
    record_obligation(fixture, method);
    switch (method) {
    case ADD:
      rc = checked(rpc_symbols[method], "friend-rpc",
                   h2_gizclaw_rpc_friend_add(service, arg, FRIEND_TIMEOUT_MS,
                                             storage, object));
      break;
    case INFO:
      rc = checked(rpc_symbols[method], "friend-rpc",
                   h2_gizclaw_rpc_friend_info_get(
                       service, arg, FRIEND_TIMEOUT_MS, storage, object));
      break;
    case LIST:
      rc =
          checked(rpc_symbols[method], "friend-rpc",
                  h2_gizclaw_rpc_friend_list(service, arg, FRIEND_PAGE_LIMIT,
                                             FRIEND_TIMEOUT_MS, storage, page));
      break;
    case DELETE:
      rc = checked(rpc_symbols[method], "friend-rpc",
                   h2_gizclaw_rpc_friend_delete(service, arg, FRIEND_TIMEOUT_MS,
                                                storage, object));
      break;
    case TOKEN_CREATE:
      rc = checked(rpc_symbols[method], "friend-rpc",
                   h2_gizclaw_rpc_friend_invite_token_create(
                       service, FRIEND_TIMEOUT_MS, storage, token));
      break;
    case TOKEN_GET:
      rc = checked(rpc_symbols[method], "friend-rpc",
                   h2_gizclaw_rpc_friend_invite_token_get(
                       service, FRIEND_TIMEOUT_MS, storage, token));
      break;
    case TOKEN_CLEAR:
      rc = checked(
          rpc_symbols[method], "friend-rpc",
          h2_gizclaw_rpc_friend_invite_token_clear(service, FRIEND_TIMEOUT_MS));
      break;
    }
  }
  return rc;
}

static bool valid_relationship(const h2_gizclaw_resp_storage_t *storage,
                               const h2_gizclaw_friend_t *value) {
  return text_valid(storage, value->id, true) &&
         text_valid(storage, value->peer_public_key, true) &&
         text_valid(storage, value->workspace_name, true) &&
         text_valid(storage, value->created_at, false) &&
         text_valid(storage, value->updated_at, false) &&
         text_valid(storage, value->name, false) &&
         text_valid(storage, value->emoji, false);
}

static int verify_list(h2_gizclaw_e2e_fixture_t *fixture,
                       h2_gizclaw_resp_storage_t *storage, bool req_api,
                       uint64_t *identity, bool present) {
  char cursor[256] = {0};
  size_t found = 0u;
  const char *peer = fixture->actors[H2_GIZCLAW_E2E_FRIEND].public_key;
  for (unsigned i = 0; i < FRIEND_MAX_PAGES; ++i) {
    h2_gizclaw_friend_page_t page = {0};
    int rc = call(fixture, storage, req_api, LIST, identity, cursor, NULL,
                  &page, NULL);
    if (rc == H2_PAL_OK &&
        (page.count > FRIEND_PAGE_LIMIT ||
         (page.count != 0u &&
          ((uintptr_t)page.items % _Alignof(h2_gizclaw_friend_t) != 0u ||
           !owned(storage, page.items, page.count * sizeof(*page.items))))))
      rc = H2_PAL_ERR_INVALID_STATE;
    for (size_t j = 0; rc == H2_PAL_OK && j < page.count; ++j) {
      const h2_gizclaw_friend_t *value = &page.items[j];
      if (!valid_relationship(storage, value)) {
        rc = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      for (size_t k = 0; k < j; ++k)
        if (strcmp(value->id, page.items[k].id) == 0)
          rc = H2_PAL_ERR_INVALID_STATE;
      if (strcmp(value->peer_public_key, peer) == 0) {
        ++found;
        // Never discard cleanup ownership if deletion failed to take effect.
        fixture->friendship_created = true;
        if (!present || found != 1u ||
            strcmp(value->id, fixture->friend_id) != 0)
          rc = H2_PAL_ERR_INVALID_STATE;
      } else if (strcmp(value->id, fixture->friend_id) == 0) {
        rc = H2_PAL_ERR_INVALID_STATE;
      }
    }
    if (rc == H2_PAL_OK && page.has_next) {
      if (!text_valid(storage, page.next_cursor, true) ||
          strlen(page.next_cursor) >= sizeof(cursor) ||
          strcmp(cursor, page.next_cursor) == 0)
        rc = H2_PAL_ERR_INVALID_STATE;
      else
        memcpy(cursor, page.next_cursor, strlen(page.next_cursor) + 1u);
    }
    storage->used = 0u;
    if (rc != H2_PAL_OK)
      return proof(req_api, LIST, rc);
    if (!page.has_next)
      return proof(req_api, LIST,
                   found == (present ? 1u : 0u) ? H2_PAL_OK
                                                : H2_PAL_ERR_INVALID_STATE);
  }
  return proof(req_api, LIST, H2_PAL_ERR_NO_SPACE);
}

static int token_create_and_read(h2_gizclaw_e2e_fixture_t *fixture,
                                 h2_gizclaw_resp_storage_t *storage,
                                 bool req_api, uint64_t *identity,
                                 char token_value[512]) {
  h2_gizclaw_invite_token_t token = {0};
  char expires[128];
  int rc = call(fixture, storage, req_api, TOKEN_CREATE, identity, "", NULL,
                NULL, &token);
  if (rc == H2_PAL_OK) {
    if (!text_valid(storage, token.value, true) ||
        !text_valid(storage, token.expires_at, true) ||
        strlen(token.value) >= 512u ||
        strlen(token.expires_at) >= sizeof(expires))
      rc = H2_PAL_ERR_INVALID_STATE;
    else {
      memcpy(token_value, token.value, strlen(token.value) + 1u);
      memcpy(expires, token.expires_at, strlen(token.expires_at) + 1u);
    }
  }
  if (rc == H2_PAL_OK)
    rc = call(fixture, storage, req_api, TOKEN_GET, identity, "", NULL, NULL,
              &token);
  if (rc == H2_PAL_OK && (!text_valid(storage, token.value, true) ||
                          !text_valid(storage, token.expires_at, true) ||
                          strcmp(token.value, token_value) != 0 ||
                          strcmp(token.expires_at, expires) != 0))
    rc = H2_PAL_ERR_INVALID_STATE;
  storage->used = 0u;
  proof(req_api, TOKEN_GET, rc);
  return proof(req_api, TOKEN_CREATE, rc);
}

static int token_clear_and_read(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_gizclaw_resp_storage_t *storage,
                                bool req_api, uint64_t *identity) {
  int rc = call(fixture, storage, req_api, TOKEN_CLEAR, identity, "", NULL,
                NULL, NULL);
  h2_gizclaw_invite_token_t token = {0};
  if (rc == H2_PAL_OK)
    rc = call(fixture, storage, req_api, TOKEN_GET, identity, "", NULL, NULL,
              &token);
  if (rc == H2_PAL_OK && (token.value != NULL || token.expires_at != NULL))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    fixture->friend_invite_created = false;
  storage->used = 0u;
  return proof(req_api, TOKEN_CLEAR, rc);
}

int h2_gizclaw_e2e_run_friend(h2_gizclaw_e2e_fixture_t *fixture,
                              h2_gizclaw_resp_storage_t *storage) {
  if (fixture == NULL || storage == NULL || storage->data == NULL ||
      storage->capacity == 0u ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL ||
      fixture->actors[H2_GIZCLAW_E2E_FRIEND].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (fixture->friendship_created || fixture->friend_invite_created ||
      fixture->friend_id[0] != '\0')
    return H2_PAL_ERR_INVALID_STATE;
  const char *peer = fixture->actors[H2_GIZCLAW_E2E_FRIEND].public_key;
  if (peer[0] == '\0' ||
      memchr(peer, '\0', sizeof(fixture->actors[0].public_key)) == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  storage->used = 0u;
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, FRIEND_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_profile_t profile = {0};
  int rc = checked(
      "h2_gizclaw_rpc_profile_get", "friend-profile",
      h2_gizclaw_rpc_profile_get(fixture->actors[H2_GIZCLAW_E2E_FRIEND].service,
                                 FRIEND_TIMEOUT_MS, &profile));
  if (rc != H2_PAL_OK)
    return rc;
  if ((profile.has_name &&
       memchr(profile.name, '\0', sizeof(profile.name)) == NULL) ||
      (profile.has_emoji &&
       memchr(profile.emoji, '\0', sizeof(profile.emoji)) == NULL))
    return H2_PAL_ERR_INVALID_STATE;
  uint64_t identity = 100u;
  for (unsigned api = 0u; rc == H2_PAL_OK && api < 2u; ++api) {
    const bool req_api = api == 0u;
    char token_value[512] = {0};
    rc = token_create_and_read(fixture, storage, req_api, &identity,
                               token_value);
    h2_gizclaw_friend_t object = {0};
    if (rc == H2_PAL_OK)
      rc = call(fixture, storage, req_api, ADD, &identity, token_value, &object,
                NULL, NULL);
    if (rc == H2_PAL_OK) {
      /* The pinned server projects FriendObject.name as the other Peer's
       * public key. Reject a different ID before saving a deletion target. */
      if (!valid_relationship(storage, &object) ||
          strcmp(object.id, peer) != 0 ||
          strcmp(object.peer_public_key, peer) != 0 ||
          strlen(object.id) >= sizeof(fixture->friend_id))
        rc = H2_PAL_ERR_INVALID_STATE;
      else
        memcpy(fixture->friend_id, object.id, strlen(object.id) + 1u);
    }
    if (rc == H2_PAL_OK)
      rc = call(fixture, storage, req_api, INFO, &identity, fixture->friend_id,
                &object, NULL, NULL);
    if (rc == H2_PAL_OK &&
        (!text_valid(storage, object.id, true) ||
         strcmp(object.id, fixture->friend_id) != 0 ||
         !text_valid(storage, object.peer_public_key, true) ||
         strcmp(object.peer_public_key, peer) != 0 ||
         !text_valid(storage, object.name, false) ||
         !text_valid(storage, object.emoji, false) ||
         (profile.has_name
              ? object.name == NULL || strcmp(object.name, profile.name) != 0
              : object.name != NULL) ||
         (profile.has_emoji
              ? object.emoji == NULL || strcmp(object.emoji, profile.emoji) != 0
              : object.emoji != NULL)))
      rc = H2_PAL_ERR_INVALID_STATE;
    proof(req_api, INFO, rc);
    if (rc == H2_PAL_OK)
      rc = verify_list(fixture, storage, req_api, &identity, true);
    proof(req_api, ADD, rc);
    if (rc == H2_PAL_OK)
      rc = token_clear_and_read(fixture, storage, req_api, &identity);
    if (rc == H2_PAL_OK)
      rc = call(fixture, storage, req_api, DELETE, &identity,
                fixture->friend_id, &object, NULL, NULL);
    if (rc == H2_PAL_OK && (!valid_relationship(storage, &object) ||
                            strcmp(object.id, fixture->friend_id) != 0 ||
                            strcmp(object.peer_public_key, peer) != 0))
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK)
      rc = verify_list(fixture, storage, req_api, &identity, false);
    proof(req_api, DELETE, rc);
    if (rc == H2_PAL_OK) {
      fixture->friendship_created = false;
      fixture->friend_id[0] = '\0';
    }
    storage->used = 0u;
  }
  return rc;
}
