#include "h2_gizclaw_e2e_group.h"
// Keep test operations and assertions enabled in optimized builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
struct h2_gizclaw_req {
  enum method method;
  char arg[512];
  unsigned phase;
};
struct model {
  bool exists, token, member;
  char name[256], workspace[256], display[64];
  h2_gizclaw_friend_group_role_t role;
};
static struct {
  h2_gizclaw_e2e_fixture_t fixture;
  h2_gizclaw_req_t request;
  struct model remote, reply;
  unsigned stages, fail, budgets, budget_fail, replies, fault_at, fault;
  unsigned mutations, mutation_fail, pages, releases, cancels;
  uint64_t last_id;
  bool live, emit;
} state;
static int step(void) {
  return ++state.stages == state.fail ? H2_PAL_ERR_IO : H2_PAL_OK;
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f == &state.fixture && ms == 30000u);
  return ++state.budgets != state.budget_fail;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc ? "FAIL" : "PASS", rc);
}
static void check(h2_gizclaw_service_t *service, enum method m,
                  h2_gizclaw_str_t name, uint32_t timeout) {
  assert(service == state.fixture.actors[m == JOIN ? 2 : 0].service);
  assert(timeout == 30000u &&
         name.len == strlen(state.fixture.friend_group_name));
  assert(!memcmp(name.data, state.fixture.friend_group_name, name.len));
}
static int execute(enum method m, const char *arg) {
  struct model before = state.remote;
  bool mutation = true;
  switch (m) {
  case CREATE:
    assert(state.fixture.friend_group_created && !state.remote.exists);
    state.remote.exists = true;
    strcpy(state.remote.name, state.fixture.friend_group_name);
    snprintf(state.remote.workspace, sizeof(state.remote.workspace),
             "workspace-%u", state.mutations);
    strcpy(state.remote.display, arg);
    break;
  case PUT:
    assert(state.remote.exists);
    strcpy(state.remote.display, arg);
    break;
  case TOKEN_CREATE:
    assert(state.fixture.friend_group_invite_created && state.remote.exists);
    state.remote.token = true;
    break;
  case JOIN:
    assert(state.fixture.friend_group_member_joined && state.remote.token &&
           !strcmp(arg, "secret-token"));
    state.remote.member = true;
    state.remote.role = H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER;
    break;
  case TOKEN_CLEAR:
    state.remote.token = false;
    break;
  case MEMBER_PUT:
    assert(state.remote.member &&
           !strcmp(state.fixture.friend_group_member_id, "membership-id"));
    state.remote.role = H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN;
    break;
  case MEMBER_DELETE:
    assert(state.remote.member &&
           state.remote.role == H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN);
    state.remote.member = false;
    break;
  case DELETE:
    assert(state.remote.exists && !state.remote.member && !state.remote.token);
    assert(!state.fixture.friend_group_member_joined &&
           !state.fixture.friend_group_invite_created);
    state.remote.exists = false;
    break;
  default:
    mutation = false;
    break;
  }
  state.reply = (m == DELETE || m == MEMBER_DELETE) ? before : state.remote;
  if (mutation && ++state.mutations == state.mutation_fail)
    state.remote = before;
  return m == GET && !state.remote.exists ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}
static char *save(h2_gizclaw_resp_storage_t *s, const char *v) {
  size_t len = strlen(v) + 1u;
  assert(s->used + len <= s->capacity);
  char *p = (char *)s->data + s->used;
  memcpy(p, v, len);
  s->used += len;
  return p;
}
static h2_gizclaw_friend_group_t group(h2_gizclaw_resp_storage_t *s,
                                       bool target, enum method m) {
  return (h2_gizclaw_friend_group_t){
      .name = save(s, target ? state.reply.name : "unrelated"),
      .workspace_name =
          save(s, target ? state.reply.workspace : "other-workspace"),
      .display_name = save(s, target ? state.reply.display : "other"),
      .description = save(s, "H2 E2E group description"),
      .my_role = !target       ? (h2_gizclaw_friend_group_role_t)127
                 : m == DELETE ? 0
                 : m == JOIN   ? H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER
                               : H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER};
}
static h2_gizclaw_friend_group_member_t member(h2_gizclaw_resp_storage_t *s,
                                               bool target) {
  return (h2_gizclaw_friend_group_member_t){
      .id = save(s, target ? "membership-id" : "owner-id"),
      .friend_group_name = save(s, state.reply.name),
      .peer_public_key =
          save(s, target ? "member-public-key" : "owner-public-key"),
      .role = target ? state.reply.role : H2_GIZCLAW_FRIEND_GROUP_ROLE_OWNER};
}
static void corrupt(h2_gizclaw_resp_storage_t *s, char **required,
                    unsigned fault) {
  switch (fault) {
  case 1:
    *required = NULL;
    break;
  case 2:
    *required = "external";
    break;
  case 3:
    *required = (char *)s->data + s->capacity - 1u;
    s->used = s->capacity;
    break;
  case 4:
    *required = save(s, "wrong");
    break;
  case 5:
    s->used = s->capacity + 1u;
    break;
  }
}
static void response(enum method m, const char *arg,
                     h2_gizclaw_resp_storage_t *s, void *out) {
  if (m == TOKEN_CLEAR)
    return;
  assert(s && s->used == 0u && out);
  memset(s->data, 0xa5, s->capacity);
  unsigned fault = ++state.replies == state.fault_at ? state.fault : 0u;
  switch (m) {
  case CREATE:
  case GET:
  case PUT:
  case DELETE:
  case JOIN: {
    h2_gizclaw_friend_group_t *g = out;
    *g = group(s, true, m);
    corrupt(s, &g->name, fault);
    if (fault == 6u)
      g->workspace_name = NULL;
    if (fault == 7u) {
      if (m == DELETE)
        g->workspace_name = save(s, "wrong-workspace");
      else
        g->my_role = H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED;
    }
    if (fault == 8u)
      g->description = "external";
    if (fault == 9u)
      g->display_name = save(s, "wrong-display");
    break;
  }
  case TOKEN_CREATE:
  case TOKEN_GET: {
    h2_gizclaw_invite_token_t *t = out;
    *t = (h2_gizclaw_invite_token_t){0};
    if (state.reply.token) {
      t->value = save(s, "secret-token");
      t->expires_at = save(s, "2030-01-01T00:00:00Z");
      corrupt(s, &t->value, fault);
      if (fault == 6u)
        t->expires_at = NULL;
      if (fault == 7u)
        t->expires_at = "external";
      if (fault == 8u)
        t->value = save(s, "");
      if (fault == 9u)
        t->expires_at = save(s, "wrong-expiry");
    } else if (fault)
      t->value = save(s, "unexpected-active-token");
    break;
  }
  case MEMBER_PUT:
  case MEMBER_DELETE: {
    h2_gizclaw_friend_group_member_t *v = out;
    *v = member(s, true);
    corrupt(s, &v->id, fault);
    if (fault == 6u)
      v->peer_public_key = NULL;
    if (fault == 7u)
      v->role = H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER;
    if (fault == 8u)
      v->friend_group_name = save(s, "wrong-group");
    if (fault == 9u)
      v->peer_public_key = save(s, "wrong-peer");
    break;
  }
  case LIST:
  case MEMBER_LIST: {
    bool second = arg[0] != '\0';
    bool next = state.pages && (state.pages != 1u || !second);
    bool target =
        state.pages == 1u ? second : state.pages != 2u && state.pages != 3u;
    if (m == MEMBER_LIST)
      target &= state.reply.member;
    size_t count = target ? 2u : 1u;
    char *next_cursor = NULL;
    if (m == LIST) {
      h2_gizclaw_friend_group_page_t *p = out;
      p->items = (h2_gizclaw_friend_group_t *)s->data;
      s->used = 3u * sizeof(*p->items);
      p->items[0] = group(s, false, m);
      if (target)
        p->items[1] = group(s, true, m);
      p->count = count;
      p->has_next = next;
      if (fault <= 5u && target)
        corrupt(s, &p->items[1].name, fault);
      else if (fault)
        p->count = 33u;
      if (state.pages == 4u) {
        p->items[2] = p->items[1];
        p->count = 3u;
      }
      if (next)
        next_cursor =
            save(s, state.pages == 3u && !strcmp(arg, "a") ? "b" : "a");
      p->next_cursor = next_cursor;
      if (fault == 7u) {
        p->count = 1u;
        p->items = (void *)(s->data + 1u);
      }
      if (fault == 8u) {
        p->count = 1u;
        p->items = NULL;
      }
      if (fault == 9u) {
        p->count = count;
        p->has_next = true;
        p->next_cursor = "external";
      }
    } else {
      h2_gizclaw_friend_group_member_page_t *p = out;
      p->items = (h2_gizclaw_friend_group_member_t *)s->data;
      s->used = 3u * sizeof(*p->items);
      p->items[0] = member(s, false);
      if (target)
        p->items[1] = member(s, true);
      p->count = count;
      p->has_next = next;
      if (fault == 4u && target)
        p->items[1].friend_group_name = save(s, "wrong-group");
      else if (fault <= 5u && target)
        corrupt(s, &p->items[1].id, fault);
      else if (fault)
        p->count = 33u;
      if (next)
        next_cursor =
            save(s, state.pages == 3u && !strcmp(arg, "a") ? "b" : "a");
      p->next_cursor = next_cursor;
      if (fault == 7u) {
        p->count = 1u;
        p->items = (void *)(s->data + 1u);
      }
      if (fault == 8u) {
        p->count = 1u;
        p->items = NULL;
      }
      if (fault == 9u) {
        p->count = count;
        p->has_next = true;
        p->next_cursor = "external";
      }
    }
    break;
  }
  default:
    assert(false);
  }
}
static int create(enum method m, h2_gizclaw_service_t *service, uint64_t id,
                  h2_gizclaw_str_t name, h2_gizclaw_str_t arg, uint32_t timeout,
                  h2_gizclaw_req_t **out) {
  check(service, m, name, timeout);
  assert(id > state.last_id && !state.live);
  state.last_id = id;
  int rc = step();
  *out = NULL;
  if (rc == H2_PAL_OK) {
    state.live = true;
    state.request.method = m;
    state.request.phase = 1u;
    assert(arg.len < sizeof(state.request.arg));
    memcpy(state.request.arg, arg.data, arg.len);
    state.request.arg[arg.len] = '\0';
    *out = &state.request;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(r == &state.request && state.live && r->phase == 1u && !user &&
         !input_read && !output_write);
  r->phase = 2u;
  return step();
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t ms) {
  assert(r == &state.request && r->phase == 2u && ms == 30000u);
  r->phase = 3u;
  int rc = step();
  return rc == H2_PAL_OK ? execute(r->method, r->arg) : rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(r == &state.request && state.live);
  ++state.cancels;
  return H2_PAL_ERR_BUSY;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  assert(r == &state.request && state.live);
  ++state.releases;
  state.live = false;
}
static int parse(const h2_gizclaw_req_t *r, enum method m,
                 h2_gizclaw_resp_storage_t *s, void *out) {
  assert(r == &state.request && state.live && r->phase == 3u && r->method == m);
  int rc = step();
  if (rc == H2_PAL_OK)
    response(m, r->arg, s, out);
  return rc;
}
static int direct(enum method m, h2_gizclaw_service_t *service,
                  h2_gizclaw_str_t name, h2_gizclaw_str_t arg, uint32_t ms,
                  h2_gizclaw_resp_storage_t *s, void *out) {
  check(service, m, name, ms);
  assert(!state.live);
  char argument[512];
  assert(arg.len < sizeof(argument));
  memcpy(argument, arg.data, arg.len);
  argument[arg.len] = '\0';
  int rc = step();
  if (rc == H2_PAL_OK)
    rc = execute(m, argument);
  if (rc == H2_PAL_OK)
    response(m, argument, s, out);
  return rc;
}
#define EMPTY h2_gizclaw_e2e_str("")
#define SIMPLE(method, op, type)                                               \
  h2_pal_result_t h2_gizclaw_req_create_friend_group_##method(                 \
      h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,             \
      uint32_t ms, h2_gizclaw_req_t **out) {                                   \
    return create(op, s, id, name, EMPTY, ms, out);                            \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_resp_parse_friend_group_##method(                 \
      const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s, type *out) {    \
    return parse(r, op, s, out);                                               \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_rpc_friend_group_##method(                        \
      h2_gizclaw_service_t *s, h2_gizclaw_str_t name, uint32_t ms,             \
      h2_gizclaw_resp_storage_t *arena, type *out) {                           \
    return direct(op, s, name, EMPTY, ms, arena, out);                         \
  }
SIMPLE(get, GET, h2_gizclaw_friend_group_t)
SIMPLE(delete, DELETE, h2_gizclaw_friend_group_t)
SIMPLE(invite_token_create, TOKEN_CREATE, h2_gizclaw_invite_token_t)
SIMPLE(invite_token_get, TOKEN_GET, h2_gizclaw_invite_token_t)
#define METADATA(method, op)                                                   \
  h2_pal_result_t h2_gizclaw_req_create_friend_group_##method(                 \
      h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,             \
      h2_gizclaw_str_t display, h2_gizclaw_str_t desc, uint32_t ms,            \
      h2_gizclaw_req_t **out) {                                                \
    assert(desc.len == strlen("H2 E2E group description"));                    \
    return create(op, s, id, name, display, ms, out);                          \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_resp_parse_friend_group_##method(                 \
      const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,                 \
      h2_gizclaw_friend_group_t *out) {                                        \
    return parse(r, op, s, out);                                               \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_rpc_friend_group_##method(                        \
      h2_gizclaw_service_t *s, h2_gizclaw_str_t name,                          \
      h2_gizclaw_str_t display, h2_gizclaw_str_t desc, uint32_t ms,            \
      h2_gizclaw_resp_storage_t *arena, h2_gizclaw_friend_group_t *out) {      \
    assert(desc.len == strlen("H2 E2E group description"));                    \
    return direct(op, s, name, display, ms, arena, out);                       \
  }
METADATA(create, CREATE)
METADATA(put, PUT)
h2_pal_result_t
h2_gizclaw_req_create_friend_group_list(h2_gizclaw_service_t *s, uint64_t id,
                                        h2_gizclaw_str_t cursor, size_t limit,
                                        uint32_t ms, h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(LIST, s, id,
                h2_gizclaw_e2e_str(state.fixture.friend_group_name), cursor, ms,
                out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_list(const h2_gizclaw_req_t *r,
                                        h2_gizclaw_resp_storage_t *s,
                                        h2_gizclaw_friend_group_page_t *out) {
  return parse(r, LIST, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t cursor, size_t limit, uint32_t ms,
    h2_gizclaw_resp_storage_t *arena, h2_gizclaw_friend_group_page_t *out) {
  assert(limit == 32u);
  return direct(LIST, s, h2_gizclaw_e2e_str(state.fixture.friend_group_name),
                cursor, ms, arena, out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_join(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t token,
    h2_gizclaw_str_t name, uint32_t ms, h2_gizclaw_req_t **out) {
  return create(JOIN, s, id, name, token, ms, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_join(const h2_gizclaw_req_t *r,
                                        h2_gizclaw_resp_storage_t *s,
                                        h2_gizclaw_friend_group_t *out) {
  return parse(r, JOIN, s, out);
}
h2_pal_result_t
h2_gizclaw_rpc_friend_group_join(h2_gizclaw_service_t *s,
                                 h2_gizclaw_str_t token, h2_gizclaw_str_t name,
                                 uint32_t ms, h2_gizclaw_resp_storage_t *arena,
                                 h2_gizclaw_friend_group_t *out) {
  return direct(JOIN, s, name, token, ms, arena, out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_clear(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name, uint32_t ms,
    h2_gizclaw_req_t **out) {
  return create(TOKEN_CLEAR, s, id, name, EMPTY, ms, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_clear(
    const h2_gizclaw_req_t *r) {
  return parse(r, TOKEN_CLEAR, NULL, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_clear(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, uint32_t ms) {
  return direct(TOKEN_CLEAR, s, name, EMPTY, ms, NULL, NULL);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_list(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t ms,
    h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(MEMBER_LIST, s, id, name, cursor, ms, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_list(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_friend_group_member_page_t *out) {
  return parse(r, MEMBER_LIST, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t ms, h2_gizclaw_resp_storage_t *arena,
    h2_gizclaw_friend_group_member_page_t *out) {
  assert(limit == 32u);
  return direct(MEMBER_LIST, s, name, cursor, ms, arena, out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_put(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t member_id, h2_gizclaw_friend_group_role_t role,
    uint32_t ms, h2_gizclaw_req_t **out) {
  assert(role == H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN &&
         member_id.len == strlen("membership-id") &&
         !memcmp(member_id.data, "membership-id", member_id.len));
  return create(MEMBER_PUT, s, id, name, EMPTY, ms, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_put(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_friend_group_member_t *out) {
  return parse(r, MEMBER_PUT, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_put(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role, uint32_t ms,
    h2_gizclaw_resp_storage_t *arena, h2_gizclaw_friend_group_member_t *out) {
  assert(role == H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN &&
         member_id.len == strlen("membership-id") &&
         !memcmp(member_id.data, "membership-id", member_id.len));
  return direct(MEMBER_PUT, s, name, EMPTY, ms, arena, out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_delete(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t member_id, uint32_t ms, h2_gizclaw_req_t **out) {
  assert(member_id.len == strlen("membership-id") &&
         !memcmp(member_id.data, "membership-id", member_id.len));
  return create(MEMBER_DELETE, s, id, name, EMPTY, ms, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_delete(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_friend_group_member_t *out) {
  return parse(r, MEMBER_DELETE, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_delete(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, h2_gizclaw_str_t member_id,
    uint32_t ms, h2_gizclaw_resp_storage_t *arena,
    h2_gizclaw_friend_group_member_t *out) {
  assert(member_id.len == strlen("membership-id") &&
         !memcmp(member_id.data, "membership-id", member_id.len));
  return direct(MEMBER_DELETE, s, name, EMPTY, ms, arena, out);
}

static void scenario(unsigned fail, unsigned budget, unsigned fault_at,
                     unsigned fault, unsigned mutation, unsigned pages,
                     bool emit) {
  memset(&state, 0, sizeof(state));
  state.fail = fail;
  state.budget_fail = budget;
  state.fault_at = fault_at;
  state.fault = fault;
  state.mutation_fail = mutation;
  state.pages = pages;
  state.emit = emit;
  state.fixture.actors[0].service = (void *)&state.remote;
  state.fixture.actors[2].service = (void *)&state.reply;
  strcpy(state.fixture.actors[2].public_key, "member-public-key");
  strcpy(state.fixture.run_prefix, "test");
  _Alignas(h2_gizclaw_friend_group_member_t) uint8_t arena[16384];
  h2_gizclaw_resp_storage_t storage = {.data = arena,
                                       .capacity = sizeof(arena)};
  if (emit) {
    puts("H2_GIZCLAW_E2E case=rpc stage=coverage-begin");
    puts("H2_GIZCLAW_E2E case=rpc/group stage=coverage-begin");
  }
  int rc = h2_gizclaw_e2e_run_group_management(&state.fixture, &storage);
  if (emit) {
    printf("H2_GIZCLAW_E2E case=rpc/group stage=coverage-end status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
    printf("H2_GIZCLAW_E2E case=rpc stage=coverage-end status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
  }
  bool success = !fail && !budget && !fault_at && !mutation && pages <= 1u;
  if ((rc == H2_PAL_OK) != success)
    fprintf(stderr,
            "fail=%u budget=%u reply=%u fault=%u mutation=%u pages=%u rc=%d "
            "stages=%u budgets=%u replies=%u mutations=%u\n",
            fail, budget, fault_at, fault, mutation, pages, rc, state.stages,
            state.budgets, state.replies, state.mutations);
  assert((rc == H2_PAL_OK) == success && !state.live && storage.used == 0u);
  if (fail)
    assert(rc == H2_PAL_ERR_IO && state.stages == fail);
  if (budget)
    assert(rc == H2_PAL_ERR_TIMEOUT && state.budgets == budget);
  if (pages == 3u)
    assert(rc == H2_PAL_ERR_NO_SPACE && state.budgets == 36u);
  if (state.remote.exists)
    assert(state.fixture.friend_group_created &&
           !strcmp(state.remote.name, state.fixture.friend_group_name));
  if (state.remote.member)
    assert(state.fixture.friend_group_member_joined);
  if (state.remote.token)
    assert(state.fixture.friend_group_invite_created);
  if (success) {
    assert(state.fixture.friend_group_created &&
           !state.fixture.friend_group_member_joined &&
           !state.fixture.friend_group_invite_created);
    assert(!strcmp(state.fixture.friend_group_name, "test-group") &&
           !strcmp(state.remote.name, "test-group"));
  }
  if (state.fixture.friend_group_created ||
      state.fixture.friend_group_invite_created ||
      state.fixture.friend_group_member_joined) {
    unsigned previous_stages = state.stages;
    char name[256];
    strcpy(name, state.fixture.friend_group_name);
    assert(h2_gizclaw_e2e_run_group_management(&state.fixture, &storage) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(state.stages == previous_stages &&
           !strcmp(name, state.fixture.friend_group_name));
  }
}
static void test_prefix_boundaries(void) {
  const size_t cap = sizeof(state.fixture.run_prefix);
  const size_t limit = sizeof(state.fixture.friend_group_name) - sizeof("-group-req");
  const size_t lengths[] = {0u, limit, limit + 1u, cap - 1u, cap};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    memset(&state, 0, sizeof(state));
    state.fixture.actors[0].service = (void *)&state.remote;
    state.fixture.actors[2].service = (void *)&state.reply;
    strcpy(state.fixture.actors[2].public_key, "member-public-key");
    memset(state.fixture.run_prefix, 'n', lengths[i]);
    strcpy(state.fixture.friend_group_name, "untouched");
    _Alignas(h2_gizclaw_friend_group_member_t) uint8_t arena[16384];
    h2_gizclaw_resp_storage_t storage = {.data = arena, .capacity = sizeof(arena)};
    int rc = h2_gizclaw_e2e_run_group_management(&state.fixture, &storage);
    assert(!state.live);
    if (lengths[i] == limit) {
      assert(rc == H2_PAL_OK && state.remote.exists);
      assert(state.fixture.friend_group_created && storage.used == 0u);
      assert(strlen(state.fixture.friend_group_name) == limit + strlen("-group"));
      assert(memcmp(state.fixture.friend_group_name, state.fixture.run_prefix, limit) == 0);
      assert(strcmp(state.fixture.friend_group_name + limit, "-group") == 0);
      assert(strcmp(state.remote.name, state.fixture.friend_group_name) == 0);
    } else {
      assert(rc == ((!lengths[i] || lengths[i] == cap)
                        ? H2_PAL_ERR_INVALID_STATE : H2_PAL_ERR_NO_SPACE));
      assert(!state.stages && !state.budgets && !state.remote.exists);
      assert(!state.fixture.friend_group_created);
      assert(strcmp(state.fixture.friend_group_name, "untouched") == 0);
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 7) {
    scenario((unsigned)atoi(argv[1]), (unsigned)atoi(argv[2]),
             (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]),
             (unsigned)atoi(argv[5]), (unsigned)atoi(argv[6]), true);
    return 0;
  }
  scenario(0, 0, 0, 0, 0, 0, false);
  unsigned stages = state.stages, budgets = state.budgets,
           replies = state.replies, mutations = state.mutations;
  printf("GROUP_BOUNDARY stages=%u budgets=%u replies=%u mutations=%u\n",
         stages, budgets, replies, mutations);
  for (unsigned i = 1; i <= stages; ++i)
    scenario(i, 0, 0, 0, 0, 0, false);
  for (unsigned i = 1; i <= budgets; ++i)
    scenario(0, i, 0, 0, 0, 0, false);
  for (unsigned i = 1; i <= replies; ++i)
    for (unsigned f = 1; f <= 9; ++f)
      scenario(0, 0, i, f, 0, 0, false);
  for (unsigned i = 1; i <= mutations; ++i)
    scenario(0, 0, 0, 0, i, 0, false);
  for (unsigned i = 1; i <= 5; ++i)
    scenario(0, 0, 0, 0, 0, i, false);
  test_prefix_boundaries();
  return 0;
}
