#include "h2_app_test_mem.h"
#include "h2_gizclaw_e2e_resource.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Consumer fault injection only: this does not constitute allocator.live_blocks Resource proof.
 * Library concurrency/pagination implementation tests remain in libs/gizclaw.
 */
struct h2_gizclaw_resource {
  h2_gizclaw_resource_snapshot_t snapshot;
  h2_gizclaw_contact_t contact;
  char name[256], display[64];
};
static int fault, creates, closes, destroys, executes, accepted;
static h2_app_test_mem_t allocator;
static void *allocate(void *u, size_t n) {
  (void)u;
  return h2_pal_mem_alloc(&allocator.api, n);
}
static void release(void *u, void *p) {
  (void)u;
  h2_pal_mem_free(&allocator.api, p);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f && ms == 30000u);
  return fault != 8;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  (void)symbol;
  if (!strcmp(stage, "resource_execute-assert") && rc == H2_PAL_OK)
    ++accepted;
}
h2_pal_result_t
h2_gizclaw_resource_create(const h2_gizclaw_resource_config_t *c,
                           h2_gizclaw_resource_t **out) {
  assert(c->page_size == 1 && c->max_items == 64 && c->storage_bytes == 65536);
  *out = allocate(NULL, sizeof(**out));
  memset(*out, 0, sizeof(**out));
  (*out)->snapshot.kind = c->kind;
  (*out)->snapshot.stale = fault != 1;
  ++creates;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_resource_snapshot(h2_gizclaw_resource_t *r,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_resource_snapshot_t *out) {
  *out = r->snapshot;
  if (out->kind == H2_GIZCLAW_RESOURCE_CONTACTS && out->data.contacts.count) {
    assert(storage->capacity >= sizeof(h2_gizclaw_contact_t) + 512);
    h2_gizclaw_contact_t *c = (h2_gizclaw_contact_t *)storage->data;
    char *name = (char *)(c + 1), *display = name + 256;
    *c = r->contact;
    strcpy(name, r->name);
    strcpy(display, r->display);
    c->name = name;
    c->display_name = display;
    out->data.contacts.items = c;
  }
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_resource_execute(h2_gizclaw_resource_t *r,
                            const h2_gizclaw_resource_command_t *c,
                            uint32_t ms) {
  assert(ms == 30000);
  if (r->snapshot.closed)
    return fault == 7 ? H2_PAL_OK : H2_PAL_ERR_CLOSED;
  ++executes;
  if (fault == 9 && c->operation == H2_GIZCLAW_RESOURCE_CONTACT_CREATE)
    return H2_PAL_ERR_TIMEOUT;
  r->snapshot.valid = true;
  r->snapshot.stale = fault == 2;
  ++r->snapshot.revision;
  ++r->snapshot.data_revision;
  if (c->operation == H2_GIZCLAW_RESOURCE_CONTACT_CREATE ||
      c->operation == H2_GIZCLAW_RESOURCE_CONTACT_UPDATE) {
    strcpy(r->name, c->name);
    strcpy(r->display, c->display_name);
    if (fault == 3 && c->operation == H2_GIZCLAW_RESOURCE_CONTACT_UPDATE)
      strcpy(r->display, "wrong");
    r->contact.phone_number = "+12025550123";
    r->snapshot.data.contacts.count = 1;
  }
  if (c->operation == H2_GIZCLAW_RESOURCE_CONTACT_DELETE && fault != 4)
    r->snapshot.data.contacts.count = 0;
  if (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_NAME) {
    r->snapshot.data.profile.has_name = true;
    strcpy(r->snapshot.data.profile.name, c->text);
  }
  if (c->operation == H2_GIZCLAW_RESOURCE_PROFILE_EMOJI) {
    r->snapshot.data.profile.has_emoji = true;
    strcpy(r->snapshot.data.profile.emoji, fault == 5 ? "wrong" : c->text);
  }
  if (r->snapshot.kind == H2_GIZCLAW_RESOURCE_POINTS) {
    r->snapshot.balance_valid = fault != 6;
    r->snapshot.balance_result = fault == 6 ? H2_PAL_ERR_IO : H2_PAL_OK;
    if (c->operation == H2_GIZCLAW_RESOURCE_REFRESH)
      r->snapshot.data.points.transactions.has_next = true;
    else {
      assert(c->operation == H2_GIZCLAW_RESOURCE_LOAD_MORE);
      r->snapshot.data.points.transactions.has_next = false;
    }
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resource_close(h2_gizclaw_resource_t *r) {
  r->snapshot.closed = true;
  r->snapshot.stale = true;
  ++closes;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resource_destroy(h2_gizclaw_resource_t **r) {
  if (fault == 10)
    return H2_PAL_ERR_BUSY;
  if (*r) {
    ++destroys;
    release(NULL, *r);
    *r = NULL;
  }
  return H2_PAL_OK;
}
int main(void) {
  h2_app_test_mem_init(&allocator, NULL);
  for (fault = 0; fault <= 10; ++fault) {
    creates = closes = destroys = executes = accepted = allocator.live_blocks = 0;
    h2_runtime_t runtime = {0};
    h2_gizclaw_e2e_fixture_t f = {.allocator = &allocator.api, .runtime = &runtime};
    f.actors[0].service = (void *)&f;
    strcpy(f.run_prefix, "resource-test");
    int rc = h2_gizclaw_e2e_run_resource(&f);
    if (fault == 0) {
      assert(rc == H2_PAL_OK && creates == 4 && destroys == 4 && accepted == 1);
      assert(!f.contact_created && executes == 10);
    } else {
      assert(rc != H2_PAL_OK && accepted == 0);
      if (fault == 3 || fault == 4 || fault == 9)
        assert(f.contact_created);
    }
    if (fault == 10) {
      assert(f.case_state && f.case_cleanup && allocator.live_blocks > 0);
      fault = 0;
      assert(f.case_cleanup(&f) == H2_PAL_OK);
      fault = 10;
    }
    assert(!f.case_state && !f.case_cleanup && allocator.live_blocks == 0 &&
           destroys == creates);
  }
  return 0;
}
