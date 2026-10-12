#ifdef NDEBUG
#undef NDEBUG
#endif
#include "pref_fixture.h"
#include "h2_pal_e2e.h"
#include "h2_sqlite.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned live_buffers;
static void *alloc_mem(void *user, size_t size) {
  (void)user;
  void *p = malloc(size);
  if (p != NULL) ++live_buffers;
  return p;
}
static void free_mem(void *user, void *p) {
  (void)user;
  if (p != NULL) { assert(live_buffers > 0u); --live_buffers; free(p); }
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = alloc_mem, .free = free_mem};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static const uint8_t first_image[32] = {1u}, next_image[32] = {2u};

static void assert_canary(const h2_pal_pref_api_t *pref) {
  h2_pal_pref_namespace_t *ns = NULL;
  uint32_t value = 0u;
  assert(h2_pal_pref_open(pref, "device-config", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->get_u32(ns, "canary", &value) == 0 && value == 0x12345678u);
  assert(ns->close(ns) == 0);
}
static void old_complete(const h2_pal_pref_api_t *pref) {
  h2_pal_pref_namespace_t *ns = NULL;
  assert(h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->clear(ns) == 0);
  assert(ns->set_blob(ns, "image", first_image, sizeof(first_image)) == 0);
  assert(ns->set_u32(ns, "phase", 3u) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  assert(h2_pal_pref_open(pref, "h2e2eprefdata", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "stale", 99u) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
}

typedef struct fault fault_t;
typedef struct proxy {
  h2_pal_pref_namespace_t api;
  h2_pal_pref_namespace_t *inner;
  fault_t *owner;
  int control;
} proxy_t;
struct fault { h2_pal_pref_api_t api; const h2_pal_pref_api_t *inner; int kind; unsigned active; };
static int inject(fault_t *f, int kind) {
  if (f->kind != kind) return 0;
  f->kind = 0;
  return 1;
}
static int close_proxy(h2_pal_pref_namespace_t *raw) {
  proxy_t *p = raw->user;
  int fail = inject(p->owner, p->control ? 10 : 9);
  int rc = p->inner->close(p->inner);
  assert(p->owner->active > 0u); --p->owner->active;
  free(p);
  return fail ? H2_PAL_ERR_IO : rc;
}
static int read_proxy(h2_pal_pref_namespace_t *raw, const h2_pal_mem_api_t *m,
                      const char *key, void **data, size_t *size) {
  proxy_t *p = raw->user;
  if (inject(p->owner, 3)) return H2_PAL_ERR_IO;
  return p->inner->get_blob(p->inner, m, key, data, size);
}
static int clear_proxy(h2_pal_pref_namespace_t *raw) {
  proxy_t *p = raw->user;
  if (inject(p->owner, p->control ? 6 : 4)) return H2_PAL_ERR_IO;
  return p->inner->clear(p->inner);
}
static int commit_proxy(h2_pal_pref_namespace_t *raw) {
  proxy_t *p = raw->user;
  if (inject(p->owner, p->control ? 8 : 5)) return H2_PAL_ERR_IO;
  return p->inner->commit(p->inner);
}
static int write_proxy(h2_pal_pref_namespace_t *raw, const char *key,
                       const void *data, size_t size) {
  proxy_t *p = raw->user;
  if (inject(p->owner, 7)) return H2_PAL_ERR_IO;
  return p->inner->set_blob(p->inner, key, data, size);
}
static int open_proxy(void *user, const char *name, h2_pal_pref_open_mode_t mode,
                      h2_pal_pref_namespace_t **out) {
  fault_t *f = user;
  int control = strcmp(name, "h2e2eprefctl") == 0;
  assert(control || strcmp(name, "h2e2eprefdata") == 0);
  assert(mode == H2_PAL_PREF_OPEN_READ_WRITE);
  if (inject(f, control ? 1 : 2)) return H2_PAL_ERR_IO;
  proxy_t *p = calloc(1u, sizeof(*p)); assert(p != NULL);
  int rc = h2_pal_pref_open(f->inner, name, mode, &p->inner);
  if (rc != 0) { free(p); return rc; }
  p->owner = f; p->control = control;
  p->api = (h2_pal_pref_namespace_t){.user = p, .close = close_proxy,
      .get_blob = read_proxy, .clear = clear_proxy, .commit = commit_proxy,
      .set_blob = write_proxy};
  ++f->active; *out = &p->api;
  return 0;
}
static const h2_pal_pref_vtable_t fault_vtable = {.open = open_proxy};

int main(void) {
  char database[512];
  const char *tmp = getenv("TEST_TMPDIR");
  snprintf(database, sizeof(database), "%s/pref-fixture-%ld.sqlite",
           tmp == NULL ? "/tmp" : tmp, (long)getpid());
  const h2_sqlite_config_t cfg = {.path = database};
  h2_sqlite_t *db = NULL;
  assert(h2_sqlite_create(&cfg, &db) == 0);
  const h2_pal_pref_api_t *pref = h2_sqlite_pref_api(db);
  h2_pal_pref_namespace_t *ns = NULL;
  assert(h2_pal_pref_open(pref, "device-config", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "canary", 0x12345678u) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  h2_sqlite_destroy(db);

  /* Real persisted providers reopen across seed/verify/clean/empty boots. */
  const unsigned expected[] = {17u, 11u, 3u, 1u};
  for (unsigned boot = 0u; boot < 4u; ++boot) {
    assert(h2_sqlite_create(&cfg, &db) == 0); pref = h2_sqlite_pref_api(db);
    int reset = -1;
    assert(h2_mosaico_pref_fixture_prepare(pref, &mem, first_image, &reset) == 0);
    assert(reset == (boot == 0u));
    h2_runtime_t rt = {.mem = &mem, .pref = pref};
    h2_pal_e2e_result_t result;
    const h2_pal_e2e_config_t suite = {.suite_mask = H2_PAL_E2E_SUITE_PREF};
    assert(h2_pal_e2e_run(&rt, &suite, &result) == 0);
    assert(result.selected == expected[boot] && result.passed == expected[boot] && result.failed == 0u);
    assert(result.action == (boot < 2u ? H2_PAL_E2E_ACTION_REBOOT : H2_PAL_E2E_ACTION_NONE));
    assert_canary(pref); assert(live_buffers == 0u);
    h2_sqlite_destroy(db);
  }
  assert(h2_sqlite_create(&cfg, &db) == 0); pref = h2_sqlite_pref_api(db);
  int reset = -1;
  assert(h2_mosaico_pref_fixture_prepare(NULL, &mem, first_image, &reset) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_mosaico_pref_fixture_prepare(pref, NULL, first_image, &reset) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_mosaico_pref_fixture_prepare(pref, &mem, NULL, &reset) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_mosaico_pref_fixture_prepare(pref, &mem, first_image, NULL) == H2_PAL_ERR_INVALID_ARG);
  old_complete(pref);
  assert(h2_mosaico_pref_fixture_prepare(pref, &mem, next_image, &reset) == 0 && reset == 1);
  assert(h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  uint32_t phase = 99u;
  assert(ns->get_u32(ns, "phase", &phase) == H2_PAL_ERR_NOT_FOUND);
  assert(ns->close(ns) == 0);
  /* Migration must rerun the actual shared registry, not just clear a marker. */
  for (unsigned boot = 0u; boot < 4u; ++boot) {
    assert(h2_mosaico_pref_fixture_prepare(pref, &mem, next_image, &reset) == 0 && reset == 0);
    h2_runtime_t rt = {.mem = &mem, .pref = pref};
    h2_pal_e2e_result_t result;
    const h2_pal_e2e_config_t suite = {.suite_mask = H2_PAL_E2E_SUITE_PREF};
    assert(h2_pal_e2e_run(&rt, &suite, &result) == 0);
    assert(result.selected == expected[boot] && result.passed == expected[boot] && result.failed == 0u);
  }
  /* A matching image must not silently reset a corrupt in-progress phase. */
  assert(h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_u32(ns, "phase", 99u) == 0 && ns->commit(ns) == 0 && ns->close(ns) == 0);
  assert(h2_mosaico_pref_fixture_prepare(pref, &mem, next_image, &reset) == 0 && reset == 0);
  h2_runtime_t bad_rt = {.mem = &mem, .pref = pref};
  h2_pal_e2e_result_t bad_result;
  const h2_pal_e2e_config_t bad_suite = {.suite_mask = H2_PAL_E2E_SUITE_PREF};
  assert(h2_pal_e2e_run(&bad_rt, &bad_suite, &bad_result) == H2_PAL_ERR_IO && bad_result.failed > 0u);
  assert(h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_blob(ns, "image", first_image, 1u) == 0 && ns->commit(ns) == 0 && ns->close(ns) == 0);
  assert(h2_mosaico_pref_fixture_prepare(pref, &mem, next_image, &reset) == H2_PAL_ERR_IO && reset == 0);
  assert(live_buffers == 0u); assert_canary(pref);
  /* Each provider error is surfaced; every opened namespace/buffer is released.
   * A subsequent successful preparation cannot inherit an old complete phase. */
  for (int kind = 1; kind <= 10; ++kind) {
    old_complete(pref);
    fault_t fault = {.inner = pref, .kind = kind};
    fault.api = (h2_pal_pref_api_t){.user = &fault, .vtable = &fault_vtable};
    assert(h2_mosaico_pref_fixture_prepare(&fault.api, &mem, next_image, &reset) == H2_PAL_ERR_IO);
    assert(reset == 0 && fault.active == 0u && fault.kind == 0 && live_buffers == 0u);
    assert(h2_mosaico_pref_fixture_prepare(pref, &mem, next_image, &reset) == 0);
    assert(h2_pal_pref_open(pref, "h2e2eprefctl", H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
    assert(ns->get_u32(ns, "phase", &phase) == H2_PAL_ERR_NOT_FOUND && ns->close(ns) == 0);
    assert_canary(pref);
  }
  h2_sqlite_destroy(db); assert(unlink(database) == 0);
  puts("Pref fixture identity, real reboot phases, config preservation and ten failure paths PASS");
  return 0;
}
