#include "h2_gizclaw_e2e_group.h"

#include <stdio.h>
#include <string.h>

#define TIMEOUT 30000u
#define LIMIT 32u
enum method {
  CREATE,
  GET,
  PUT,
  LIST,
  DELETE,
  TOKEN_CREATE,
  TOKEN_GET,
  TOKEN_CLEAR,
  JOIN,
  MEMBER_LIST,
  MEMBER_PUT,
  MEMBER_DELETE
};
static const char *const methods[] = {"friend_group_create",
                                      "friend_group_get",
                                      "friend_group_put",
                                      "friend_group_list",
                                      "friend_group_delete",
                                      "friend_group_invite_token_create",
                                      "friend_group_invite_token_get",
                                      "friend_group_invite_token_clear",
                                      "friend_group_join",
                                      "friend_group_member_list",
                                      "friend_group_member_put",
                                      "friend_group_member_delete"};
union response {
  h2_gizclaw_friend_group_t group;
  h2_gizclaw_friend_group_page_t groups;
  h2_gizclaw_invite_token_t token;
  h2_gizclaw_friend_group_member_t member;
  h2_gizclaw_friend_group_member_page_t members;
};
static int record(bool req, enum method m, const char *stage, int rc) {
  char symbol[112];
  snprintf(symbol, sizeof(symbol), "h2_gizclaw_%s_%s",
           req ? "resp_parse" : "rpc", methods[m]);
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}
static int proof(bool req, enum method m, int rc) {
  char stage[80];
  snprintf(stage, sizeof(stage), "%s-assert", methods[m]);
  return record(req, m, stage, rc);
}
static bool owns(const h2_gizclaw_resp_storage_t *s, const void *p, size_t n) {
  uintptr_t b = (uintptr_t)s->data, a = (uintptr_t)p;
  return p && s->used <= s->capacity && a >= b && a - b <= s->used &&
         n <= s->used - (a - b);
}
static bool text(const h2_gizclaw_resp_storage_t *s, const char *p,
                 bool required) {
  return !p ? !required
            : owns(s, p, 1u) && (!required || p[0]) &&
                  memchr(p, '\0',
                         s->used - ((uintptr_t)p - (uintptr_t)s->data));
}
static void obligation(h2_gizclaw_e2e_fixture_t *f, enum method m) {
  if (m == CREATE)
    f->friend_group_created = true;
  if (m == TOKEN_CREATE)
    f->friend_group_invite_created = true;
  if (m == JOIN)
    f->friend_group_member_joined = true;
}
static int call(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                bool req, enum method m, uint64_t *id, const char *arg,
                union response *r) {
  memset(r, 0, sizeof(*r));
  s->used = 0u;
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service =
      f->actors[m == JOIN ? H2_GIZCLAW_E2E_GROUP_MEMBER : H2_GIZCLAW_E2E_OWNER]
          .service;
  h2_gizclaw_str_t name = h2_gizclaw_e2e_str(f->friend_group_name);
  h2_gizclaw_str_t value = h2_gizclaw_e2e_str(arg);
  h2_gizclaw_str_t member = h2_gizclaw_e2e_str(f->friend_group_member_id);
  h2_gizclaw_str_t description = h2_gizclaw_e2e_str("H2 E2E group description");
  h2_gizclaw_req_t *request = NULL;
  int rc = H2_PAL_ERR_INVALID_STATE;
  if (!req) {
    obligation(f, m);
    switch (m) {
    case CREATE:
      rc = h2_gizclaw_rpc_friend_group_create(service, name, value, description,
                                              TIMEOUT, s, &r->group);
      break;
    case GET:
      rc =
          h2_gizclaw_rpc_friend_group_get(service, name, TIMEOUT, s, &r->group);
      break;
    case PUT:
      rc = h2_gizclaw_rpc_friend_group_put(service, name, value, description,
                                           TIMEOUT, s, &r->group);
      break;
    case LIST:
      rc = h2_gizclaw_rpc_friend_group_list(service, value, LIMIT, TIMEOUT, s,
                                            &r->groups);
      break;
    case DELETE:
      rc = h2_gizclaw_rpc_friend_group_delete(service, name, TIMEOUT, s,
                                              &r->group);
      break;
    case TOKEN_CREATE:
      rc = h2_gizclaw_rpc_friend_group_invite_token_create(
          service, name, TIMEOUT, s, &r->token);
      break;
    case TOKEN_GET:
      rc = h2_gizclaw_rpc_friend_group_invite_token_get(service, name, TIMEOUT,
                                                        s, &r->token);
      break;
    case TOKEN_CLEAR:
      rc = h2_gizclaw_rpc_friend_group_invite_token_clear(service, name,
                                                          TIMEOUT);
      break;
    case JOIN:
      rc = h2_gizclaw_rpc_friend_group_join(service, value, name, TIMEOUT, s,
                                            &r->group);
      break;
    case MEMBER_LIST:
      rc = h2_gizclaw_rpc_friend_group_member_list(service, name, value, LIMIT,
                                                   TIMEOUT, s, &r->members);
      break;
    case MEMBER_PUT:
      rc = h2_gizclaw_rpc_friend_group_member_put(
          service, name, member, H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN, TIMEOUT, s,
          &r->member);
      break;
    case MEMBER_DELETE:
      rc = h2_gizclaw_rpc_friend_group_member_delete(service, name, member,
                                                     TIMEOUT, s, &r->member);
      break;
    }
    return record(false, m, "group-rpc", rc);
  }
  uint64_t identity = (*id)++;
  switch (m) {
  case CREATE:
    rc = h2_gizclaw_req_create_friend_group_create(
        service, identity, name, value, description, TIMEOUT, &request);
    break;
  case GET:
    rc = h2_gizclaw_req_create_friend_group_get(service, identity, name,
                                                TIMEOUT, &request);
    break;
  case PUT:
    rc = h2_gizclaw_req_create_friend_group_put(service, identity, name, value,
                                                description, TIMEOUT, &request);
    break;
  case LIST:
    rc = h2_gizclaw_req_create_friend_group_list(service, identity, value,
                                                 LIMIT, TIMEOUT, &request);
    break;
  case DELETE:
    rc = h2_gizclaw_req_create_friend_group_delete(service, identity, name,
                                                   TIMEOUT, &request);
    break;
  case TOKEN_CREATE:
    rc = h2_gizclaw_req_create_friend_group_invite_token_create(
        service, identity, name, TIMEOUT, &request);
    break;
  case TOKEN_GET:
    rc = h2_gizclaw_req_create_friend_group_invite_token_get(
        service, identity, name, TIMEOUT, &request);
    break;
  case TOKEN_CLEAR:
    rc = h2_gizclaw_req_create_friend_group_invite_token_clear(
        service, identity, name, TIMEOUT, &request);
    break;
  case JOIN:
    rc = h2_gizclaw_req_create_friend_group_join(service, identity, value, name,
                                                 TIMEOUT, &request);
    break;
  case MEMBER_LIST:
    rc = h2_gizclaw_req_create_friend_group_member_list(
        service, identity, name, value, LIMIT, TIMEOUT, &request);
    break;
  case MEMBER_PUT:
    rc = h2_gizclaw_req_create_friend_group_member_put(
        service, identity, name, member, H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN,
        TIMEOUT, &request);
    break;
  case MEMBER_DELETE:
    rc = h2_gizclaw_req_create_friend_group_member_delete(
        service, identity, name, member, TIMEOUT, &request);
    break;
  }
  char symbol[112];
  snprintf(symbol, sizeof(symbol), "h2_gizclaw_req_create_%s", methods[m]);
  h2_gizclaw_e2e_evidence(symbol, "group-req", rc);
  if (rc == H2_PAL_OK) {
    obligation(f, m);
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_do", "group-req", rc);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_req_wait(request, TIMEOUT);
    h2_gizclaw_e2e_evidence("h2_gizclaw_req_wait", "group-req", rc);
  }
  if (rc == H2_PAL_OK) {
    switch (m) {
    case CREATE:
      rc = h2_gizclaw_resp_parse_friend_group_create(request, s, &r->group);
      break;
    case GET:
      rc = h2_gizclaw_resp_parse_friend_group_get(request, s, &r->group);
      break;
    case PUT:
      rc = h2_gizclaw_resp_parse_friend_group_put(request, s, &r->group);
      break;
    case LIST:
      rc = h2_gizclaw_resp_parse_friend_group_list(request, s, &r->groups);
      break;
    case DELETE:
      rc = h2_gizclaw_resp_parse_friend_group_delete(request, s, &r->group);
      break;
    case TOKEN_CREATE:
      rc = h2_gizclaw_resp_parse_friend_group_invite_token_create(request, s,
                                                                  &r->token);
      break;
    case TOKEN_GET:
      rc = h2_gizclaw_resp_parse_friend_group_invite_token_get(request, s,
                                                               &r->token);
      break;
    case TOKEN_CLEAR:
      rc = h2_gizclaw_resp_parse_friend_group_invite_token_clear(request);
      break;
    case JOIN:
      rc = h2_gizclaw_resp_parse_friend_group_join(request, s, &r->group);
      break;
    case MEMBER_LIST:
      rc = h2_gizclaw_resp_parse_friend_group_member_list(request, s,
                                                          &r->members);
      break;
    case MEMBER_PUT:
      rc =
          h2_gizclaw_resp_parse_friend_group_member_put(request, s, &r->member);
      break;
    case MEMBER_DELETE:
      rc = h2_gizclaw_resp_parse_friend_group_member_delete(request, s,
                                                            &r->member);
      break;
    }
    record(true, m, "group-req", rc);
  }
  if (request) {
    if (rc != H2_PAL_OK)
      (void)h2_gizclaw_req_cancel(request);
    h2_gizclaw_req_release(request);
  }
  return rc;
}
static bool valid_group(const h2_gizclaw_resp_storage_t *s,
                        const h2_gizclaw_friend_group_t *g) {
  return text(s, g->name, true) && text(s, g->workspace_name, true) &&
         text(s, g->display_name, false) && text(s, g->description, false);
}
static bool matches(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                    const h2_gizclaw_friend_group_t *g, const char *display,
                    h2_gizclaw_friend_group_role_t role) {
  return valid_group(s, g) && !strcmp(g->name, f->friend_group_name) &&
         text(s, g->display_name, true) && !strcmp(g->display_name, display) &&
         text(s, g->description, true) &&
         !strcmp(g->description, "H2 E2E group description") &&
         (!f->friend_group_workspace_name[0] ||
          !strcmp(g->workspace_name, f->friend_group_workspace_name)) &&
         (role == H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED ||
          g->my_role == role);
}
static bool valid_member(h2_gizclaw_e2e_fixture_t *f,
                         h2_gizclaw_resp_storage_t *s,
                         const h2_gizclaw_friend_group_member_t *m) {
  return text(s, m->id, true) && text(s, m->peer_public_key, true) &&
         text(s, m->friend_group_name, true) &&
         !strcmp(m->friend_group_name, f->friend_group_name) &&
         text(s, m->created_at, false) && text(s, m->updated_at, false);
}
static int pages(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                 bool req, bool members, uint64_t *id, const char *display,
                 bool present, h2_gizclaw_friend_group_role_t role) {
  char cursor[256] = {0}, found_id[H2_GIZCLAW_E2E_NAME_CAPACITY] = {0};
  unsigned found = 0u;
  enum method method = members ? MEMBER_LIST : LIST;
  for (unsigned page = 0u; page < 32u; ++page) {
    union response r;
    int rc = call(f, s, req, method, id, cursor, &r);
    if (rc != H2_PAL_OK)
      return rc;
    size_t count = members ? r.members.count : r.groups.count;
    void *items = members ? (void *)r.members.items : (void *)r.groups.items;
    size_t item_size =
        members ? sizeof(*r.members.items) : sizeof(*r.groups.items);
    size_t align = members ? _Alignof(h2_gizclaw_friend_group_member_t)
                           : _Alignof(h2_gizclaw_friend_group_t);
    bool next = members ? r.members.has_next : r.groups.has_next;
    const char *next_cursor =
        members ? r.members.next_cursor : r.groups.next_cursor;
    if (s->used > s->capacity || count > LIMIT ||
        (count &&
         (!owns(s, items, count * item_size) || (uintptr_t)items % align)))
      rc = H2_PAL_ERR_FORMAT;
    for (size_t i = 0; rc == H2_PAL_OK && i < count; ++i) {
      const char *key;
      bool target;
      if (members) {
        const h2_gizclaw_friend_group_member_t *m = &r.members.items[i];
        if (!valid_member(f, s, m)) {
          rc = H2_PAL_ERR_FORMAT;
          break;
        }
        key = m->id;
        target = !strcmp(m->peer_public_key,
                         f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key);
        if (target && (!present || m->role != role ||
                       (f->friend_group_member_id[0] &&
                        strcmp(m->id, f->friend_group_member_id))))
          rc = H2_PAL_ERR_FORMAT;
        for (size_t j = 0u; j < i; ++j)
          if (!strcmp(key, r.members.items[j].id))
            rc = H2_PAL_ERR_FORMAT;
      } else {
        const h2_gizclaw_friend_group_t *g = &r.groups.items[i];
        if (!valid_group(s, g)) {
          rc = H2_PAL_ERR_FORMAT;
          break;
        }
        key = g->name;
        target = !strcmp(key, f->friend_group_name);
        if (target &&
            !matches(f, s, g, display, H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER))
          rc = H2_PAL_ERR_FORMAT;
        for (size_t j = 0u; j < i; ++j)
          if (!strcmp(key, r.groups.items[j].name))
            rc = H2_PAL_ERR_FORMAT;
      }
      if (target) {
        if (members)
          f->friend_group_member_joined = true;
        if (++found != 1u || strlen(key) >= sizeof(found_id))
          rc = H2_PAL_ERR_FORMAT;
        else
          memcpy(found_id, key, strlen(key) + 1u);
      }
    }
    if (rc == H2_PAL_OK && next) {
      if (!text(s, next_cursor, true) ||
          strlen(next_cursor) >= sizeof(cursor) || !strcmp(next_cursor, cursor))
        rc = H2_PAL_ERR_FORMAT;
      else
        memcpy(cursor, next_cursor, strlen(next_cursor) + 1u);
    }
    s->used = 0u;
    if (rc != H2_PAL_OK)
      return proof(req, method, rc);
    if (!next) {
      rc = found == (present ? 1u : 0u) ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
      if (rc == H2_PAL_OK && members && present)
        memcpy(f->friend_group_member_id, found_id, strlen(found_id) + 1u);
      return proof(req, method, rc);
    }
  }
  return proof(req, method, H2_PAL_ERR_NO_SPACE);
}
static int create_group(h2_gizclaw_e2e_fixture_t *f,
                        h2_gizclaw_resp_storage_t *s, bool req, uint64_t *id) {
  union response r;
  int rc = call(f, s, req, CREATE, id, "H2 E2E group", &r);
  if (rc == H2_PAL_OK && (!matches(f, s, &r.group, "H2 E2E group",
                                   H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER) ||
                          strlen(r.group.workspace_name) >=
                              sizeof(f->friend_group_workspace_name)))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    strcpy(f->friend_group_workspace_name, r.group.workspace_name);
    rc = call(f, s, req, GET, id, "", &r);
  }
  if (rc == H2_PAL_OK && !matches(f, s, &r.group, "H2 E2E group",
                                  H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER))
    rc = H2_PAL_ERR_FORMAT;
  proof(req, GET, rc);
  return proof(req, CREATE, rc);
}
static int cycle(h2_gizclaw_e2e_fixture_t *f, h2_gizclaw_resp_storage_t *s,
                 bool req, uint64_t *id) {
  union response r;
  int rc = create_group(f, s, req, id);
  if (rc != H2_PAL_OK)
    return rc;
  rc = call(f, s, req, PUT, id, "H2 renamed group", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.group, "H2 renamed group",
                                  H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK)
    rc = call(f, s, req, GET, id, "", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.group, "H2 renamed group",
                                  H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER))
    rc = H2_PAL_ERR_FORMAT;
  if (proof(req, PUT, rc) != H2_PAL_OK)
    return rc;
  rc = pages(f, s, req, false, id, "H2 renamed group", true, 0);
  if (rc != H2_PAL_OK)
    return rc;
  char token[512], expiry[128];
  rc = call(f, s, req, TOKEN_CREATE, id, "", &r);
  if (rc == H2_PAL_OK &&
      (!text(s, r.token.value, true) || !text(s, r.token.expires_at, true) ||
       strlen(r.token.value) >= sizeof(token) ||
       strlen(r.token.expires_at) >= sizeof(expiry)))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    strcpy(token, r.token.value);
    strcpy(expiry, r.token.expires_at);
    rc = call(f, s, req, TOKEN_GET, id, "", &r);
  }
  if (rc == H2_PAL_OK &&
      (!text(s, r.token.value, true) || !text(s, r.token.expires_at, true) ||
       strcmp(r.token.value, token) || strcmp(r.token.expires_at, expiry)))
    rc = H2_PAL_ERR_FORMAT;
  proof(req, TOKEN_GET, rc);
  if (proof(req, TOKEN_CREATE, rc) != H2_PAL_OK)
    return rc;
  rc = call(f, s, req, JOIN, id, token, &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.group, "H2 renamed group",
                                  H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK)
    rc = pages(f, s, req, true, id, "", true,
               H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER);
  if (proof(req, JOIN, rc) != H2_PAL_OK)
    return rc;
  rc = call(f, s, req, TOKEN_CLEAR, id, "", &r);
  if (rc == H2_PAL_OK)
    rc = call(f, s, req, TOKEN_GET, id, "", &r);
  if (rc == H2_PAL_OK && (r.token.value || r.token.expires_at))
    rc = H2_PAL_ERR_FORMAT;
  if (proof(req, TOKEN_CLEAR, rc) != H2_PAL_OK)
    return rc;
  f->friend_group_invite_created = false;
  for (unsigned remove = 0u; remove < 2u; ++remove) {
    enum method m = remove ? MEMBER_DELETE : MEMBER_PUT;
    rc = call(f, s, req, m, id, "", &r);
    if (rc == H2_PAL_OK &&
        (!valid_member(f, s, &r.member) ||
         strcmp(r.member.id, f->friend_group_member_id) ||
         strcmp(r.member.peer_public_key,
                f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key) ||
         r.member.role != H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN))
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) {
      if (remove)
        f->friend_group_member_joined = false;
      rc = pages(f, s, req, true, id, "", !remove,
                 H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN);
    }
    if (proof(req, m, rc) != H2_PAL_OK)
      return rc;
  }
  rc = call(f, s, req, DELETE, id, "", &r);
  if (rc == H2_PAL_OK && !matches(f, s, &r.group, "H2 renamed group",
                                  H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    /* Independent readback avoids counting an expected failed req_wait as
     * another successful GET chain. Delete is acknowledged before probing. */
    rc = call(f, s, false, GET, id, "", &r);
    rc = rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK
         : rc == H2_PAL_OK          ? H2_PAL_ERR_INVALID_STATE
                                    : rc;
  }
  if (proof(req, DELETE, rc) == H2_PAL_OK)
    f->friend_group_created = false;
  return rc;
}
int h2_gizclaw_e2e_run_group_management(h2_gizclaw_e2e_fixture_t *f,
                                        h2_gizclaw_resp_storage_t *s) {
  if (!f || !s || !s->data || !s->capacity || !f->actors[0].service ||
      !f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].service)
    return H2_PAL_ERR_INVALID_ARG;
  if (f->friend_group_created || f->friend_group_invite_created ||
      f->friend_group_member_joined || f->isolation_group_pending ||
      !f->run_prefix[0] ||
      !memchr(f->run_prefix, '\0', sizeof(f->run_prefix)) ||
      !f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key[0] ||
      !memchr(f->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key, '\0',
              sizeof(f->actors[0].public_key)))
    return H2_PAL_ERR_INVALID_STATE;
  const size_t prefix_len = strlen(f->run_prefix);
  if (prefix_len > sizeof(f->friend_group_name) - sizeof("-group-req"))
    return H2_PAL_ERR_NO_SPACE;
  uint64_t identity = 301u;
  int rc = H2_PAL_OK;
  for (unsigned api = 0u; api < 3u && rc == H2_PAL_OK; ++api) {
    const char *suffix = api == 0u ? "-group-req"
                        : api == 1u ? "-group-rpc" : "-group";
    memcpy(f->friend_group_name, f->run_prefix, prefix_len);
    memcpy(f->friend_group_name + prefix_len, suffix, strlen(suffix) + 1u);
    f->friend_group_workspace_name[0] = f->friend_group_member_id[0] = '\0';
    rc = api == 2u ? create_group(f, s, false, &identity)
                   : cycle(f, s, api == 0u, &identity);
  }
  s->used = 0u;
  return rc;
}
