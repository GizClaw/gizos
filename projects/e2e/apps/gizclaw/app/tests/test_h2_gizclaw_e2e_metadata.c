#include "h2_gizclaw_e2e_metadata.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { NORMAL, EMPTY, FOREIGN_KEYS, TOO_MANY, BAD_PROFILE, BAD_REVISION,
       DUPLICATE_KEY, BAD_CURSOR, EMPTY_CURSOR, FOREVER_PAGES, FOREIGN_VALUE,
       TOO_LONG_VALUE, CHANGED_VALUE, CHANGED_REVISION, EMPTY_VALUE,
       FOREIGN_ITEMS, BAD_PUBLIC_KEY, BAD_PUBLIC_NAME, BAD_PUBLIC_EMOJI,
       PUBLIC_DUPLICATE, DEADLINE, CALL_ERROR };
static unsigned mode, calls, fail_at, assertions, releases, list_calls, get_calls;
static bool emit;
struct h2_gizclaw_req { unsigned kind; bool started, completed; } request;
static bool alive;
static int service;
static int step(void) { return ++calls == fail_at ? H2_PAL_ERR_IO : H2_PAL_OK; }
static char *copy(h2_gizclaw_resp_storage_t *s, const char *v) {
  size_t n = strlen(v) + 1u;
  assert(s->used + n <= s->capacity);
  char *p = (char *)s->data + s->used; memcpy(p, v, n); s->used += n; return p;
}
static void *array(h2_gizclaw_resp_storage_t *s, size_t n) {
  assert(s->used == 0u && n <= s->capacity); s->used = n; return s->data;
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *v) {
  return (h2_gizclaw_str_t){v, strlen(v)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t n) {
  assert(f && n == 30000u); return mode != DEADLINE;
}
void h2_gizclaw_e2e_evidence(const char *sym, const char *stage, int rc) {
  if (strstr(stage, "-assert") && rc == H2_PAL_OK) ++assertions;
  if (emit) printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n",
      sym, stage, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
static int create(h2_gizclaw_service_t *s, unsigned k, uint32_t t, h2_gizclaw_req_t **out) {
  assert(s && t == 30000u && !alive); *out = NULL;
  int rc = step(); if (rc) return rc;
  request = (struct h2_gizclaw_req){.kind = k}; *out = &request; alive = true; return 0;
}
h2_pal_result_t h2_gizclaw_req_create_app_config_list(h2_gizclaw_service_t *s,
    uint64_t id, h2_gizclaw_str_t c, size_t n, uint32_t t, h2_gizclaw_req_t **out) {
  (void)c; assert(id == 60u && n == 16u); return create(s, 0u, t, out);
}
h2_pal_result_t h2_gizclaw_req_create_app_config_get(h2_gizclaw_service_t *s,
    uint64_t id, h2_gizclaw_str_t k, uint32_t t, h2_gizclaw_req_t **out) {
  assert(id == 61u && k.len == 7u && !memcmp(k.data, "fixture", 7u)); return create(s, 1u, t, out);
}
h2_pal_result_t h2_gizclaw_req_create_public_profile_get(h2_gizclaw_service_t *s,
    uint64_t id, const h2_gizclaw_str_t *keys, size_t n, uint32_t t, h2_gizclaw_req_t **out) {
  assert(id == 62u && n == 2u && keys[0].len == keys[1].len); return create(s, 2u, t, out);
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r, void *u,
    h2_gizclaw_req_input_read_fn in, h2_gizclaw_req_output_write_fn out,
    h2_gizclaw_req_complete_fn done) {
  assert(alive && r == &request && !u && !in && !out && !done);
  int rc = step(); if (!rc) r->started = true; return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t t) {
  assert(alive && r->started && t == 30000u); int rc = step();
  if (!rc) r->completed = true; return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) { assert(alive && r); return 0; }
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  if (r) { assert(alive); alive = false; ++releases; }
}
static int list(h2_gizclaw_resp_storage_t *s, h2_gizclaw_app_config_page_t *o) {
  ++list_calls; int rc = step(); if (rc) return rc;
  char **keys = array(s, 2u * sizeof(char *)); keys[0] = copy(s, "fixture"); keys[1] = keys[0];
  *o = (h2_gizclaw_app_config_page_t){.keys=keys, .count=1u,
      .runtime_profile_name=copy(s, mode == BAD_PROFILE ? "other" : "profile"),
      .runtime_profile_revision=copy(s, mode == BAD_REVISION ? "" : "v1")};
  if (mode == EMPTY) o->count = 0u;
  if (mode == FOREIGN_KEYS) o->keys = (char **)&request;
  if (mode == TOO_MANY) o->count = 17u;
  if (mode == DUPLICATE_KEY) o->count = 2u;
  if (mode == BAD_CURSOR || mode == EMPTY_CURSOR || mode == FOREVER_PAGES) {
    char cursor[32]; snprintf(cursor, sizeof(cursor), "%u", list_calls);
    o->has_next = true; o->next_cursor = mode == BAD_CURSOR ? "foreign" :
      copy(s, mode == EMPTY_CURSOR ? "" : cursor);
  }
  return 0;
}
static int get(h2_gizclaw_resp_storage_t *s, h2_gizclaw_app_config_value_t *o) {
  ++get_calls; int rc = step(); if (rc) return rc;
  char *value = copy(s, mode == EMPTY_VALUE ? "" : mode == CHANGED_VALUE && get_calls > 1u ? "changed" : "value");
  *o = (h2_gizclaw_app_config_value_t){.value={.data=value, .len=strlen(value)},
      .runtime_profile_name=copy(s, "profile"),
      .runtime_profile_revision=copy(s, mode == CHANGED_REVISION ? "v2" : "v1")};
  if (mode == FOREIGN_VALUE) o->value.data = "foreign";
  if (mode == TOO_LONG_VALUE) o->value.len = 4097u;
  return 0;
}
static int public_profile(h2_gizclaw_resp_storage_t *s, h2_gizclaw_public_profile_list_t *o) {
  int rc = step(); if (rc) return rc;
  h2_gizclaw_public_profile_t *p = array(s, sizeof(*p));
  *p = (h2_gizclaw_public_profile_t){
    .peer_public_key=copy(s, mode == BAD_PUBLIC_KEY ? "other" : "peer"),
    .display_name=copy(s, mode == BAD_PUBLIC_NAME ? "other" : "name"),
    .emoji=copy(s, mode == BAD_PUBLIC_EMOJI ? "other" : "emoji")};
  *o = (h2_gizclaw_public_profile_list_t){.items=p, .count=mode == PUBLIC_DUPLICATE ? 2u : 1u};
  if (mode == FOREIGN_ITEMS) o->items = (h2_gizclaw_public_profile_t *)&request;
  return 0;
}
h2_pal_result_t h2_gizclaw_resp_parse_app_config_list(const h2_gizclaw_req_t *r,
    h2_gizclaw_resp_storage_t *s, h2_gizclaw_app_config_page_t *o) {
  assert(alive && r->completed && r->kind == 0u); return list(s, o);
}
h2_pal_result_t h2_gizclaw_resp_parse_app_config_get(const h2_gizclaw_req_t *r,
    h2_gizclaw_resp_storage_t *s, h2_gizclaw_app_config_value_t *o) {
  assert(alive && r->completed && r->kind == 1u); return get(s, o);
}
h2_pal_result_t h2_gizclaw_resp_parse_public_profile_get(const h2_gizclaw_req_t *r,
    h2_gizclaw_resp_storage_t *s, h2_gizclaw_public_profile_list_t *o) {
  assert(alive && r->completed && r->kind == 2u); return public_profile(s, o);
}
h2_pal_result_t h2_gizclaw_rpc_app_config_list(h2_gizclaw_service_t *service,
    h2_gizclaw_str_t cursor, size_t n, uint32_t t, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_app_config_page_t *o) {
  assert(service && !cursor.len && n == 16u && t == 30000u); return list(s, o);
}
h2_pal_result_t h2_gizclaw_rpc_app_config_get(h2_gizclaw_service_t *service,
    h2_gizclaw_str_t key, uint32_t t, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_app_config_value_t *o) {
  assert(service && key.len == 7u && t == 30000u); return get(s, o);
}
h2_pal_result_t h2_gizclaw_rpc_public_profile_get(h2_gizclaw_service_t *service,
    const h2_gizclaw_str_t *keys, size_t n, uint32_t t, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_public_profile_list_t *o) {
  assert(service && keys && n == 2u && t == 30000u); return public_profile(s, o);
}
h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *s, uint32_t t,
    h2_gizclaw_profile_t *out) {
  assert(s && t == 30000u); int rc = step(); if (rc) return rc;
  *out = (h2_gizclaw_profile_t){.has_name=true, .has_emoji=true};
  strcpy(out->name, "name"); strcpy(out->emoji, "emoji"); return 0;
}
static int run(unsigned m, unsigned fail, bool profile) {
  mode = m; fail_at = fail; calls = assertions = releases = list_calls = get_calls = 0u; alive = false;
  h2_gizclaw_e2e_fixture_t f = {.runtime_profile_name="profile",
      .actors={{.service=(h2_gizclaw_service_t *)&service, .public_key="peer"}}};
  _Alignas(max_align_t) unsigned char data[8192];
  h2_gizclaw_resp_storage_t s = {.data=data, .capacity=sizeof(data)};
  int rc = profile ? h2_gizclaw_e2e_run_public_profile(&f, &s) : h2_gizclaw_e2e_run_app_config(&f, &s);
  assert(!alive); return rc;
}
int main(int argc, char **argv) {
  emit = argc == 2 && strcmp(argv[1], "--emit-success-evidence") == 0;
  if (emit) puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc");
  if (emit) puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/app-config");
  assert(run(NORMAL, 0u, false) == 0 && assertions == 4u && releases == 2u);
  unsigned count = calls;
  if (emit) puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc/app-config status=PASS rc=0 cleanup_rc=0");
  if (emit) puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/profile");
  assert(run(NORMAL, 0u, true) == 0 && assertions == 2u && releases == 1u);
  unsigned profile_count = calls;
  if (emit) {
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc/profile status=PASS rc=0 cleanup_rc=0");
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 cleanup_rc=0"); return 0;
  }
  for (unsigned i=1u; i<=count; ++i) assert(run(CALL_ERROR, i, false) == H2_PAL_ERR_IO);
  for (unsigned i=1u; i<=profile_count; ++i) assert(run(CALL_ERROR, i, true) == H2_PAL_ERR_IO);
  for (unsigned i=EMPTY; i<=CHANGED_REVISION; ++i) assert(run(i, 0u, false) != H2_PAL_OK);
  assert(run(EMPTY_VALUE, 0u, false) == H2_PAL_OK);
  for (unsigned i=FOREIGN_ITEMS; i<=PUBLIC_DUPLICATE; ++i) assert(run(i, 0u, true) != H2_PAL_OK);
  assert(run(DEADLINE, 0u, false) == H2_PAL_ERR_TIMEOUT);
  assert(run(DEADLINE, 0u, true) == H2_PAL_ERR_TIMEOUT);
  return 0;
}
