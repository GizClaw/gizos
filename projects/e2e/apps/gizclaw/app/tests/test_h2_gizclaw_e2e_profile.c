#include "h2_gizclaw_e2e_profile.h"

// Keep test operations and assertions enabled in optimized builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* Case-boundary double: validate actual public calls, ordering and assertions.
 * This is not a GizClaw server and produces no network performance evidence. */
struct h2_gizclaw_req {
  unsigned method;
  uint64_t identity;
  char value[257];
  bool submitted, completed;
};

static struct {
  h2_gizclaw_profile_t profile, prior_profile;
  struct h2_gizclaw_req request;
  unsigned step, fail_at, snapshots, corrupt_at, deadline_checks, expire_at;
  unsigned updates, drop_update;
  unsigned creates[3], parses[3], rpc[3], dos, waits, cancels,
      releases;
  bool alive;
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static void copy_value(char *out, size_t cap, h2_gizclaw_str_t value) {
  assert(value.data != NULL && value.len < cap);
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
}

static void snapshot(h2_gizclaw_profile_t *out) {
  *out = state.profile;
  if (++state.snapshots == state.corrupt_at) {
    out->has_name = false;
    out->has_emoji = false;
  }
}

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value) {
  return (h2_gizclaw_str_t){value, strlen(value)};
}

bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t required) {
  assert(f != NULL && required == 30000u);
  return ++state.deadline_checks != state.expire_at;
}

void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_IO ||
         rc == H2_PAL_ERR_INVALID_STATE);
}

static int create(h2_gizclaw_service_t *service, unsigned method,
                  uint64_t identity, h2_gizclaw_str_t value, uint32_t timeout,
                  h2_gizclaw_req_t **out) {
  assert(service != NULL && timeout == 30000u && !state.alive);
  *out = NULL;
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  assert(identity == 11u + state.releases);
  memset(&state.request, 0, sizeof(state.request));
  state.request.method = method;
  state.request.identity = identity;
  if (method != 0u)
    copy_value(state.request.value, sizeof(state.request.value), value);
  ++state.creates[method];
  state.alive = true;
  *out = &state.request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_create_profile_get(h2_gizclaw_service_t *service,
                                                  uint64_t id, uint32_t timeout,
                                                  h2_gizclaw_req_t **out) {
  return create(service, 0u, id, (h2_gizclaw_str_t){0}, timeout, out);
}
h2_pal_result_t h2_gizclaw_req_create_profile_put_name(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t value,
    uint32_t timeout, h2_gizclaw_req_t **out) {
  return create(service, 1u, id, value, timeout, out);
}
h2_pal_result_t h2_gizclaw_req_create_profile_put_emoji(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t value,
    uint32_t timeout, h2_gizclaw_req_t **out) {
  return create(service, 2u, id, value, timeout, out);
}

h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *req,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(state.alive && req == &state.request && !req->submitted);
  assert(user == NULL && input_read == NULL && output_write == NULL);
  int rc = step();
  if (rc == H2_PAL_OK) {
    req->submitted = true;
    ++state.dos;
  }
  return rc;
}


h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *req, uint32_t timeout) {
  assert(state.alive && req == &state.request && req->submitted &&
         timeout == 30000u);
  int rc = step();
  if (rc == H2_PAL_OK) {
    if (req->method != 0u) {
      state.prior_profile = state.profile;
      ++state.updates;
    }
    if (req->method == 1u) {
      copy_value(state.profile.name, sizeof(state.profile.name),
                 h2_gizclaw_e2e_str(req->value));
      state.profile.has_name = true;
    } else if (req->method == 2u) {
      copy_value(state.profile.emoji, sizeof(state.profile.emoji),
                 h2_gizclaw_e2e_str(req->value));
      state.profile.has_emoji = true;
    }
    req->completed = true;
    ++state.waits;
  }
  return rc;
}

static int parse(const h2_gizclaw_req_t *req, unsigned method,
                 h2_gizclaw_profile_t *out) {
  assert(state.alive && req == &state.request && req->completed &&
         req->method == method);
  int rc = step();
  if (rc == H2_PAL_OK) {
    ++state.parses[method];
    snapshot(out);
    // Echo a successful put response, but discard its persisted update.
    if (method != 0u && state.updates == state.drop_update)
      state.profile = state.prior_profile;
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_resp_parse_profile_get(const h2_gizclaw_req_t *req,
                                                  h2_gizclaw_profile_t *out) {
  return parse(req, 0u, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_name(const h2_gizclaw_req_t *req,
                                       h2_gizclaw_profile_t *out) {
  return parse(req, 1u, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_emoji(const h2_gizclaw_req_t *req,
                                        h2_gizclaw_profile_t *out) {
  return parse(req, 2u, out);
}

h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *req) {
  assert(state.alive && req == &state.request);
  ++state.cancels;
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *req) {
  if (req == NULL)
    return;
  assert(state.alive && req == &state.request);
  state.alive = false;
  ++state.releases;
}

static int rpc(h2_gizclaw_service_t *service, unsigned method,
               h2_gizclaw_str_t value, uint32_t timeout,
               h2_gizclaw_profile_t *out) {
  assert(service != NULL && timeout == 30000u && !state.alive);
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  ++state.rpc[method];
  if (method != 0u) {
    state.prior_profile = state.profile;
    ++state.updates;
  }
  if (method == 1u) {
    copy_value(state.profile.name, sizeof(state.profile.name), value);
    state.profile.has_name = true;
  } else if (method == 2u) {
    copy_value(state.profile.emoji, sizeof(state.profile.emoji), value);
    state.profile.has_emoji = true;
  }
  snapshot(out);
  if (method != 0u && state.updates == state.drop_update)
    state.profile = state.prior_profile;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *service,
                                           uint32_t timeout,
                                           h2_gizclaw_profile_t *out) {
  return rpc(service, 0u, (h2_gizclaw_str_t){0}, timeout, out);
}
h2_pal_result_t h2_gizclaw_rpc_profile_put_name(h2_gizclaw_service_t *service,
                                                h2_gizclaw_str_t value,
                                                uint32_t timeout,
                                                h2_gizclaw_profile_t *out) {
  return rpc(service, 1u, value, timeout, out);
}
h2_pal_result_t h2_gizclaw_rpc_profile_put_emoji(h2_gizclaw_service_t *service,
                                                 h2_gizclaw_str_t value,
                                                 uint32_t timeout,
                                                 h2_gizclaw_profile_t *out) {
  return rpc(service, 2u, value, timeout, out);
}

int main(void) {
  h2_gizclaw_e2e_fixture_t fixture = {0};
  fixture.actors[0].service = (void *)(uintptr_t)1u;
  strcpy(fixture.run_prefix, "isolated-profile");
  assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_OK);
  assert(!state.alive && state.releases == 4u && state.dos == 4u &&
         state.waits == 4u && state.cancels == 0u);
  for (unsigned i = 0u; i < 3u; ++i) {
    assert(state.creates[i] == (i == 0u ? 2u : 1u));
    assert(state.parses[i] == state.creates[i] && state.rpc[i] == 1u);
  }
  assert(strcmp(state.profile.name, "isolated-profile-owner-req") == 0);
  assert(strcmp(state.profile.emoji, "🧪") == 0);
  const unsigned steps = state.step, snapshots = state.snapshots,
                 deadlines = state.deadline_checks;
  assert(steps == 19u && snapshots == 7u && state.updates == 4u);
  for (unsigned i = 1u; i <= steps; ++i) {
    memset(&state, 0, sizeof(state));
    state.fail_at = i;
    assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_ERR_IO);
    assert(!state.alive && state.step == i);
    assert(state.releases ==
           state.creates[0] + state.creates[1] + state.creates[2]);
  }
  for (unsigned i = 1u; i <= snapshots; ++i) {
    memset(&state, 0, sizeof(state));
    state.corrupt_at = i;
    assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(!state.alive && state.snapshots == i);
  }
  for (unsigned i = 1u; i <= deadlines; ++i) {
    memset(&state, 0, sizeof(state));
    state.expire_at = i;
    assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_ERR_TIMEOUT);
    assert(!state.alive && state.deadline_checks == i);
  }
  for (unsigned i = 1u; i <= 4u; ++i) {
    memset(&state, 0, sizeof(state));
    state.drop_update = i;
    assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(!state.alive && state.snapshots == (i <= 2u ? 3u : 7u));
  }
  memset(&state, 0, sizeof(state));
  fixture.run_prefix[0] = '\0';
  assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(state.step == 0u);
  const size_t max_prefix = H2_GIZCLAW_E2E_NAME_CAPACITY - sizeof("-owner-rpc");
  assert(max_prefix + 1u < sizeof(fixture.run_prefix));
  memset(fixture.run_prefix, 'a', max_prefix);
  fixture.run_prefix[max_prefix] = '\0';
  assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_OK);
  assert(!state.alive &&
         strlen(state.profile.name) == H2_GIZCLAW_E2E_NAME_CAPACITY - 1u);
  memset(&state, 0, sizeof(state));
  fixture.run_prefix[max_prefix] = 'a';
  fixture.run_prefix[max_prefix + 1u] = '\0';
  assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(state.step == 0u);
  memset(&state, 0, sizeof(state));
  memset(fixture.run_prefix, 'a', sizeof(fixture.run_prefix));
  assert(h2_gizclaw_e2e_run_profile(&fixture, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(state.step == 0u);
  assert(h2_gizclaw_e2e_run_profile(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
