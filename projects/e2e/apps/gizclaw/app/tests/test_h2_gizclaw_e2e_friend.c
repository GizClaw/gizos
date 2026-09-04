#include "h2_gizclaw_e2e_friend.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum method { ADD, INFO, LIST, DELETE, TOKEN_CREATE, TOKEN_GET, TOKEN_CLEAR };
struct h2_gizclaw_req {
  enum method method;
  char argument[512];
  bool submitted, completed;
};
static int services[2];
static struct {
  h2_gizclaw_e2e_fixture_t *fixture;
  struct h2_gizclaw_req request;
  bool alive, exists, token_active, emit, absent_profile;
  unsigned step, fail_at, deadlines, expire_at, changes, discard_change;
  unsigned replies, corrupt_at, corrupt_field, pagination;
  unsigned calls[3][7], creates, releases, next_id, generation;
  char token[64];
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void copy(char *out, size_t cap, h2_gizclaw_str_t value) {
  assert(value.len < cap && (value.data != NULL || value.len == 0u));
  if (value.len)
    memcpy(out, value.data, value.len);
  out[value.len] = '\0';
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value) {
  return (h2_gizclaw_str_t){value, value == NULL ? 0u : strlen(value)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture == state.fixture && ms == 30000u);
  return ++state.deadlines != state.expire_at;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
static void check_service(h2_gizclaw_service_t *service, enum method method) {
  assert(service ==
         (h2_gizclaw_service_t *)&services[method >= TOKEN_CREATE ? 1 : 0]);
}
static char *save(h2_gizclaw_resp_storage_t *storage, const char *value) {
  size_t size = strlen(value) + 1u;
  assert(size <= storage->capacity - storage->used);
  char *out = (char *)storage->data + storage->used;
  memcpy(out, value, size);
  storage->used += size;
  return out;
}
static int perform(enum method method, const char *arg) {
  if (method == TOKEN_CREATE) {
    assert(state.fixture->friend_invite_created);
    snprintf(state.token, sizeof(state.token), "local-invite-%u",
             ++state.generation);
    if (++state.changes != state.discard_change)
      state.token_active = true;
  } else if (method == TOKEN_CLEAR) {
    assert(state.fixture->friend_invite_created);
    if (++state.changes != state.discard_change)
      state.token_active = false;
  } else if (method == ADD) {
    assert(state.fixture->friendship_created);
    assert(state.token_active && strcmp(arg, state.token) == 0);
    assert(!state.exists);
    if (++state.changes != state.discard_change)
      state.exists = true;
  } else if (method == DELETE) {
    assert(state.fixture->friendship_created && state.exists);
    assert(strcmp(arg, "friend-peer") == 0);
    if (++state.changes != state.discard_change)
      state.exists = false;
  } else if (method == INFO) {
    if (!state.exists)
      return H2_PAL_ERR_NOT_FOUND;
    assert(strcmp(arg, "friend-peer") == 0);
  }
  return H2_PAL_OK;
}
static void relationship(h2_gizclaw_resp_storage_t *storage,
                         h2_gizclaw_friend_t *out) {
  *out = (h2_gizclaw_friend_t){
      .id = save(storage, "friend-peer"),
      .peer_public_key = save(storage, "friend-peer"),
      .workspace_name = save(storage, "workspace"),
      .created_at = save(storage, "2026-09-03T00:00:00Z"),
      .updated_at = save(storage, "2026-09-03T00:00:00Z")};
}
static void respond(enum method method, const char *arg,
                    h2_gizclaw_resp_storage_t *storage,
                    h2_gizclaw_friend_t *object, h2_gizclaw_friend_page_t *page,
                    h2_gizclaw_invite_token_t *token) {
  if (method == TOKEN_CLEAR)
    return;
  assert(storage->used == 0u);
  memset(storage->data, 0xDD, storage->capacity);
  ++state.replies;
  if (method >= TOKEN_CREATE) {
    *token = (h2_gizclaw_invite_token_t){0};
    if (method == TOKEN_CREATE || state.token_active) {
      token->value = save(storage, state.token);
      token->expires_at = save(storage, "2026-09-03T01:00:00Z");
    }
    if (state.replies == state.corrupt_at) {
      switch (state.corrupt_field) {
      case 0:
        token->value = save(storage, "wrong");
        break;
      case 1:
        token->value = NULL;
        break;
      case 2:
        token->expires_at = NULL;
        break;
      case 3:
        token->expires_at = save(storage, "wrong-expiry");
        break;
      case 4:
        token->value = "outside-arena";
        break;
      case 5:
        token->value = save(storage, "");
        break;
      default:
        token->value = (char *)storage->data;
        memset(storage->data, 'x', storage->capacity);
        storage->used = storage->capacity;
        break;
      }
    }
  } else if (method == LIST) {
    *page = (h2_gizclaw_friend_page_t){0};
    if (state.pagination != 0u && state.pagination <= 3u &&
        (arg[0] == '\0' || state.pagination >= 2u)) {
      page->has_next = true;
      page->next_cursor =
          save(storage, state.pagination == 3u && strcmp(arg, "page-two") == 0
                            ? "page-one"
                            : "page-two");
    } else if (state.exists) {
      page->items = (h2_gizclaw_friend_t *)storage->data;
      page->count = state.pagination == 4u || state.pagination == 5u ? 2u : 1u;
      storage->used = page->count * sizeof(*page->items);
      relationship(storage, page->items);
      if (page->count == 2u) {
        relationship(storage, &page->items[1]);
        if (state.pagination == 5u) {
          page->items[1].id = save(storage, "unrelated-peer");
          page->items[1].peer_public_key = save(storage, "unrelated-peer");
        }
      }
      if (state.pagination == 6u && arg[0] == '\0') {
        page->has_next = true;
        page->next_cursor = save(storage, "page-two");
      }
    }
    if (state.replies == state.corrupt_at) {
      switch (state.corrupt_field) {
      case 0:
        page->items = NULL;
        page->count = 1u;
        break;
      case 1:
        page->count = 33u;
        break;
      case 2:
        page->has_next = true;
        page->next_cursor = NULL;
        break;
      case 3:
        page->has_next = true;
        page->next_cursor = save(storage, "");
        break;
      case 4:
        page->has_next = true;
        page->next_cursor = "outside-arena";
        break;
      case 5:
        page->has_next = true;
        page->next_cursor = save(storage, "aaaaaaaa");
        memset(page->next_cursor, 'x', 9u);
        break;
      case 6:
        assert(page->count == 1u);
        page->items[0].peer_public_key = save(storage, "different");
        break;
      case 7:
        page->has_next = true;
        page->next_cursor = (char *)storage->data + storage->used;
        memset(page->next_cursor, 'x', 256u);
        page->next_cursor[256] = '\0';
        storage->used += 257u;
        break;
      case 8:
        page->count = 1u;
        page->items = (h2_gizclaw_friend_t *)(storage->data + 1u);
        storage->used = 1u + sizeof(*page->items);
        break;
      }
    }
  } else {
    relationship(storage, object);
    if (method == INFO) {
      object->workspace_name = object->created_at = object->updated_at = NULL;
      if (!state.absent_profile) {
        object->name = save(storage, "Friend");
        object->emoji = save(storage, ":)");
      }
    }
    if (state.replies == state.corrupt_at) {
      switch (state.corrupt_field) {
      case 0:
        object->id = NULL;
        break;
      case 1:
        object->peer_public_key = save(storage, "different");
        break;
      case 2:
        object->id = "outside-arena";
        break;
      case 3:
        object->peer_public_key = NULL;
        break;
      case 4:
        object->workspace_name = NULL;
        break;
      case 5:
        object->name = save(storage, "wrong");
        break;
      case 6:
        object->emoji = save(storage, "wrong");
        break;
      case 8:
        object->id = save(storage, "another-peer");
        break;
      default:
        object->id = (char *)storage->data;
        memset(storage->data, 'x', storage->capacity);
        storage->used = storage->capacity;
        break;
      }
    }
  }
}
static int create(enum method method, h2_gizclaw_service_t *service,
                  uint64_t id, h2_gizclaw_str_t arg, uint32_t timeout,
                  h2_gizclaw_req_t **out) {
  check_service(service, method);
  assert(timeout == 30000u && id == state.next_id++);
  assert(!state.alive);
  ++state.calls[0][method];
  *out = NULL;
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  memset(&state.request, 0, sizeof(state.request));
  state.request.method = method;
  copy(state.request.argument, sizeof(state.request.argument), arg);
  state.alive = true;
  ++state.creates;
  *out = &state.request;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(request == &state.request && state.alive && !request->submitted);
  assert(user == NULL && input_read == NULL && output_write == NULL);
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  request->submitted = true;
  return perform(request->method, request->argument);
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout) {
  assert(request == &state.request && state.alive && request->submitted &&
         timeout == 30000u);
  int rc = step();
  request->completed = rc == H2_PAL_OK;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  assert(request == &state.request && state.alive);
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  if (request == NULL)
    return;
  assert(request == &state.request && state.alive);
  state.alive = false;
  ++state.releases;
  memset(request, 0xA5, sizeof(*request));
}
static int parse(enum method method, const h2_gizclaw_req_t *request,
                 h2_gizclaw_resp_storage_t *storage,
                 h2_gizclaw_friend_t *object, h2_gizclaw_friend_page_t *page,
                 h2_gizclaw_invite_token_t *token) {
  assert(state.alive && request == &state.request && request->completed &&
         method == request->method);
  ++state.calls[1][method];
  int rc = step();
  if (rc == H2_PAL_OK)
    respond(method, request->argument, storage, object, page, token);
  return rc;
}
static int sync_call(enum method method, h2_gizclaw_service_t *service,
                     h2_gizclaw_str_t arg, uint32_t timeout,
                     h2_gizclaw_resp_storage_t *storage,
                     h2_gizclaw_friend_t *object,
                     h2_gizclaw_friend_page_t *page,
                     h2_gizclaw_invite_token_t *token) {
  check_service(service, method);
  assert(timeout == 30000u && !state.alive);
  ++state.calls[2][method];
  char copied[512];
  copy(copied, sizeof(copied), arg);
  int rc = step();
  if (rc == H2_PAL_OK)
    rc = perform(method, copied);
  if (rc == H2_PAL_OK)
    respond(method, copied, storage, object, page, token);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *service,
                                           uint32_t timeout,
                                           h2_gizclaw_profile_t *out) {
  assert(service == (h2_gizclaw_service_t *)&services[1] && timeout == 30000u);
  *out = (h2_gizclaw_profile_t){.has_name = !state.absent_profile,
                                .name = "Friend",
                                .has_emoji = !state.absent_profile,
                                .emoji = ":)"};
  return step();
}

h2_pal_result_t h2_gizclaw_req_create_friend_add(h2_gizclaw_service_t *service,
                                                 uint64_t id,
                                                 h2_gizclaw_str_t arg,
                                                 uint32_t timeout,
                                                 h2_gizclaw_req_t **out) {

  return create(ADD, service, id, arg, timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_add(const h2_gizclaw_req_t *request,
                                 h2_gizclaw_resp_storage_t *storage,
                                 h2_gizclaw_friend_t *out) {
  return parse(ADD, request, storage, out, NULL, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_friend_add(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t arg,
                                          uint32_t timeout,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_friend_t *out) {

  return sync_call(ADD, service, arg, timeout, storage, out, NULL, NULL);
}

h2_pal_result_t h2_gizclaw_req_create_friend_info_get(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t arg,
    uint32_t timeout, h2_gizclaw_req_t **out) {

  return create(INFO, service, id, arg, timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_info_get(const h2_gizclaw_req_t *request,
                                      h2_gizclaw_resp_storage_t *storage,
                                      h2_gizclaw_friend_t *out) {
  return parse(INFO, request, storage, out, NULL, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_friend_info_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t arg, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_friend_t *out) {

  return sync_call(INFO, service, arg, timeout, storage, out, NULL, NULL);
}

h2_pal_result_t
h2_gizclaw_req_create_friend_list(h2_gizclaw_service_t *service, uint64_t id,
                                  h2_gizclaw_str_t arg, size_t limit,
                                  uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(LIST, service, id, arg, timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_list(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_friend_page_t *out) {
  return parse(LIST, request, storage, NULL, out, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_friend_list(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t arg, size_t limit,
                                           uint32_t timeout,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_friend_page_t *out) {
  assert(limit == 32u);
  return sync_call(LIST, service, arg, timeout, storage, NULL, out, NULL);
}

h2_pal_result_t
h2_gizclaw_req_create_friend_delete(h2_gizclaw_service_t *service, uint64_t id,
                                    h2_gizclaw_str_t arg, uint32_t timeout,
                                    h2_gizclaw_req_t **out) {

  return create(DELETE, service, id, arg, timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_friend_delete(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_friend_t *out) {
  return parse(DELETE, request, storage, out, NULL, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_friend_delete(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t arg,
                                             uint32_t timeout,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_friend_t *out) {

  return sync_call(DELETE, service, arg, timeout, storage, out, NULL, NULL);
}

h2_pal_result_t
h2_gizclaw_req_create_friend_invite_token_create(h2_gizclaw_service_t *service,
                                                 uint64_t id, uint32_t timeout,
                                                 h2_gizclaw_req_t **out) {

  return create(TOKEN_CREATE, service, id, (h2_gizclaw_str_t){0}, timeout, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out) {
  return parse(TOKEN_CREATE, request, storage, NULL, NULL, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_create(
    h2_gizclaw_service_t *service, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out) {

  return sync_call(TOKEN_CREATE, service, (h2_gizclaw_str_t){0}, timeout,
                   storage, NULL, NULL, out);
}

h2_pal_result_t
h2_gizclaw_req_create_friend_invite_token_get(h2_gizclaw_service_t *service,
                                              uint64_t id, uint32_t timeout,
                                              h2_gizclaw_req_t **out) {

  return create(TOKEN_GET, service, id, (h2_gizclaw_str_t){0}, timeout, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out) {
  return parse(TOKEN_GET, request, storage, NULL, NULL, out);
}
h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_get(
    h2_gizclaw_service_t *service, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out) {

  return sync_call(TOKEN_GET, service, (h2_gizclaw_str_t){0}, timeout, storage,
                   NULL, NULL, out);
}

h2_pal_result_t
h2_gizclaw_req_create_friend_invite_token_clear(h2_gizclaw_service_t *service,
                                                uint64_t id, uint32_t timeout,
                                                h2_gizclaw_req_t **out) {

  return create(TOKEN_CLEAR, service, id, (h2_gizclaw_str_t){0}, timeout, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_clear(
    const h2_gizclaw_req_t *request) {
  return parse(TOKEN_CLEAR, request, NULL, NULL, NULL, NULL);
}
h2_pal_result_t
h2_gizclaw_rpc_friend_invite_token_clear(h2_gizclaw_service_t *service,
                                         uint32_t timeout) {

  return sync_call(TOKEN_CLEAR, service, (h2_gizclaw_str_t){0}, timeout, NULL,
                   NULL, NULL, NULL);
}

static void reset(h2_gizclaw_e2e_fixture_t *fixture) {
  assert(!state.alive);
  memset(&state, 0, sizeof(state));
  memset(fixture, 0, sizeof(*fixture));
  state.fixture = fixture;
  state.next_id = 100u;
  fixture->actors[0].service = (h2_gizclaw_service_t *)&services[0];
  fixture->actors[1].service = (h2_gizclaw_service_t *)&services[1];
  strcpy(fixture->actors[1].public_key, "friend-peer");
}
static int run(h2_gizclaw_e2e_fixture_t *fixture,
               h2_gizclaw_resp_storage_t *storage) {
  int rc = h2_gizclaw_e2e_run_friend(fixture, storage);
  assert(!state.alive && state.creates == state.releases &&
         storage->used == 0u);
  if (state.exists)
    assert(fixture->friendship_created);
  if (state.token_active)
    assert(fixture->friend_invite_created);
  return rc;
}
int main(int argc, char **argv) {
  h2_gizclaw_e2e_fixture_t fixture;
  union {
    max_align_t align;
    uint8_t bytes[16384];
  } buffer;
  h2_gizclaw_resp_storage_t storage = {.data = buffer.bytes,
                                       .capacity = sizeof(buffer.bytes)};
  reset(&fixture);
  if (argc == 2 && strcmp(argv[1], "--emit-success-evidence") == 0) {
    state.emit = true;
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc");
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/friend");
    assert(run(&fixture, &storage) == H2_PAL_OK);
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc/friend status=PASS rc=0 "
         "cleanup_rc=0");
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 "
         "cleanup_rc=0");
    return 0;
  }
  assert(run(&fixture, &storage) == H2_PAL_OK);
  assert(!state.exists && !state.token_active && !fixture.friendship_created &&
         !fixture.friend_invite_created);
  const unsigned steps = state.step, deadlines = state.deadlines,
                 changes = state.changes, replies = state.replies;
  for (unsigned family = 0; family < 3u; ++family)
    for (unsigned method = 0; method < 7u; ++method)
      assert(state.calls[family][method] > 0u);
  for (unsigned failure = 1; failure <= steps; ++failure) {
    reset(&fixture);
    state.fail_at = failure;
    assert(run(&fixture, &storage) == H2_PAL_ERR_IO);
  }
  for (unsigned failure = 1; failure <= deadlines; ++failure) {
    reset(&fixture);
    state.expire_at = failure;
    assert(run(&fixture, &storage) == H2_PAL_ERR_TIMEOUT);
  }
  for (unsigned failure = 1; failure <= changes; ++failure) {
    reset(&fixture);
    state.discard_change = failure;
    assert(run(&fixture, &storage) != H2_PAL_OK);
  }
  for (unsigned reply = 1; reply <= replies; ++reply) {
    reset(&fixture);
    state.corrupt_at = reply;
    // A create token must be nonempty; a wrong but well-formed token is
    // detected by get.
    state.corrupt_field = (reply == 1u || reply == 9u) ? 1u : 0u;
    assert(run(&fixture, &storage) != H2_PAL_OK);
  }
  const unsigned token_replies[] = {1, 2, 9, 10};
  for (unsigned i = 0; i < 4u; ++i)
    for (unsigned field = 1; field <= 6u; ++field) {
      if ((token_replies[i] == 1u || token_replies[i] == 9u) && field == 3u)
        continue;
      reset(&fixture);
      state.corrupt_at = token_replies[i];
      state.corrupt_field = field;
      assert(run(&fixture, &storage) != H2_PAL_OK);
    }
  const unsigned objects[] = {3, 4, 7, 11, 12, 15};
  for (unsigned i = 0; i < 6u; ++i)
    for (unsigned field = 1; field <= 8u; ++field) {
      bool info = objects[i] == 4u || objects[i] == 12u;
      if ((field == 4u && info) || ((field == 5u || field == 6u) && !info))
        continue;
      reset(&fixture);
      state.corrupt_at = objects[i];
      state.corrupt_field = field;
      assert(run(&fixture, &storage) != H2_PAL_OK);
      if (field == 8u && (objects[i] == 3u || objects[i] == 11u))
        assert(fixture.friend_id[0] == '\0');
    }
  const unsigned pages[] = {5, 8, 13, 16};
  for (unsigned i = 0; i < 4u; ++i)
    for (unsigned field = 1; field <= 8u; ++field) {
      if (field == 6u && (pages[i] == 8u || pages[i] == 16u))
        continue;
      reset(&fixture);
      state.corrupt_at = pages[i];
      state.corrupt_field = field;
      assert(run(&fixture, &storage) != H2_PAL_OK);
    }
  reset(&fixture);
  state.pagination = 1u;
  assert(run(&fixture, &storage) == H2_PAL_OK);
  reset(&fixture);
  state.pagination = 2u;
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_STATE);
  reset(&fixture);
  state.pagination = 3u;
  assert(run(&fixture, &storage) == H2_PAL_ERR_NO_SPACE);
  reset(&fixture);
  state.pagination = 4u;
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_STATE);
  reset(&fixture);
  state.pagination = 5u;
  assert(run(&fixture, &storage) == H2_PAL_OK);
  reset(&fixture);
  state.pagination = 6u;
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_STATE);
  reset(&fixture);
  state.absent_profile = true;
  assert(run(&fixture, &storage) == H2_PAL_OK);
  reset(&fixture);
  fixture.friendship_created = true;
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_STATE &&
         state.step == 0u);
  reset(&fixture);
  fixture.friend_invite_created = true;
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_STATE &&
         state.step == 0u);
  reset(&fixture);
  fixture.actors[1].public_key[0] = '\0';
  assert(run(&fixture, &storage) == H2_PAL_ERR_INVALID_ARG);
  printf("Friend boundary: steps=%u deadlines=%u changes=%u replies=%u pass\n",
         steps, deadlines, changes, replies);
  return 0;
}
