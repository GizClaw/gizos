#include "h2_pal_e2e.h"
#include "h2_sqlite.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *test_alloc(void *user, size_t size) { (void)user; return malloc(size); }
static void *test_realloc(void *user, void *ptr, size_t size) { (void)user; return realloc(ptr, size); }
static void test_free(void *user, void *ptr) { (void)user; free(ptr); }

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};
static const h2_pal_mem_api_t s_mem = {.user = NULL, .vtable = &s_mem_vtable};

typedef struct fault_pref fault_pref_t;

typedef struct fault_namespace {
  h2_pal_pref_namespace_t base;
  h2_pal_pref_namespace_t *inner;
  fault_pref_t *owner;
} fault_namespace_t;

struct fault_pref {
  h2_pal_pref_api_t api;
  h2_pal_pref_vtable_t vtable;
  const h2_pal_pref_api_t *inner;
  int fail_set_blob_once;
  unsigned close_count;
};

static fault_namespace_t *fault_ns(h2_pal_pref_namespace_t *raw) {
  return raw == NULL ? NULL : (fault_namespace_t *)raw->user;
}

static int fault_close(h2_pal_pref_namespace_t *raw) {
  fault_namespace_t *ns = fault_ns(raw);
  int rc;
  if (ns == NULL) return H2_PAL_ERR_INVALID_ARG;
  rc = ns->inner->close(ns->inner);
  ns->owner->close_count++;
  free(ns);
  return rc;
}

static int fault_get_blob(h2_pal_pref_namespace_t *raw,
                          const h2_pal_mem_api_t *allocator,
                          const char *key, void **out_data, size_t *out_len) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->get_blob(ns->inner, allocator, key, out_data, out_len);
}

static int fault_set_blob(h2_pal_pref_namespace_t *raw, const char *key,
                          const void *data, size_t data_len) {
  fault_namespace_t *ns = fault_ns(raw);
  if (ns->owner->fail_set_blob_once) {
    ns->owner->fail_set_blob_once = 0;
    return H2_PAL_ERR_IO;
  }
  return ns->inner->set_blob(ns->inner, key, data, data_len);
}

static int fault_get_string(h2_pal_pref_namespace_t *raw,
                            const h2_pal_mem_api_t *allocator,
                            const char *key, char **out_value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->get_string(ns->inner, allocator, key, out_value);
}

static int fault_set_string(h2_pal_pref_namespace_t *raw, const char *key,
                            const char *value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->set_string(ns->inner, key, value);
}

static int fault_get_u32(h2_pal_pref_namespace_t *raw, const char *key,
                         uint32_t *out_value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->get_u32(ns->inner, key, out_value);
}

static int fault_set_u32(h2_pal_pref_namespace_t *raw, const char *key,
                         uint32_t value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->set_u32(ns->inner, key, value);
}

static int fault_get_i32(h2_pal_pref_namespace_t *raw, const char *key,
                         int32_t *out_value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->get_i32(ns->inner, key, out_value);
}

static int fault_set_i32(h2_pal_pref_namespace_t *raw, const char *key,
                         int32_t value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->set_i32(ns->inner, key, value);
}

static int fault_get_bool(h2_pal_pref_namespace_t *raw, const char *key,
                          int *out_value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->get_bool(ns->inner, key, out_value);
}

static int fault_set_bool(h2_pal_pref_namespace_t *raw, const char *key,
                          int value) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->set_bool(ns->inner, key, value);
}

static int fault_remove(h2_pal_pref_namespace_t *raw, const char *key) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->remove(ns->inner, key);
}

static int fault_clear(h2_pal_pref_namespace_t *raw) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->clear(ns->inner);
}

static int fault_commit(h2_pal_pref_namespace_t *raw) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->commit(ns->inner);
}

static int fault_iterate(h2_pal_pref_namespace_t *raw,
                         h2_pal_pref_cursor_t **cursor,
                         h2_pal_pref_entry_t *out_entry) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->iterate(ns->inner, cursor, out_entry);
}

static int fault_iterate_close(h2_pal_pref_namespace_t *raw,
                               h2_pal_pref_cursor_t **cursor) {
  fault_namespace_t *ns = fault_ns(raw);
  return ns->inner->iterate_close(ns->inner, cursor);
}

static int fault_open(void *user, const char *name_space,
                      h2_pal_pref_open_mode_t mode,
                      h2_pal_pref_namespace_t **out_namespace) {
  fault_pref_t *owner = (fault_pref_t *)user;
  fault_namespace_t *ns;
  h2_pal_pref_namespace_t *inner = NULL;
  int rc = h2_pal_pref_open(owner->inner, name_space, mode, &inner);
  if (rc != H2_PAL_OK) return rc;
  ns = (fault_namespace_t *)calloc(1u, sizeof(*ns));
  if (ns == NULL) {
    (void)inner->close(inner);
    return H2_PAL_ERR_NO_MEMORY;
  }
  ns->inner = inner;
  ns->owner = owner;
  ns->base.user = ns;
  ns->base.close = fault_close;
  ns->base.get_blob = fault_get_blob;
  ns->base.set_blob = fault_set_blob;
  ns->base.get_string = fault_get_string;
  ns->base.set_string = fault_set_string;
  ns->base.get_u32 = fault_get_u32;
  ns->base.set_u32 = fault_set_u32;
  ns->base.get_i32 = fault_get_i32;
  ns->base.set_i32 = fault_set_i32;
  ns->base.get_bool = fault_get_bool;
  ns->base.set_bool = fault_set_bool;
  ns->base.remove = fault_remove;
  ns->base.clear = fault_clear;
  ns->base.commit = fault_commit;
  ns->base.iterate = fault_iterate;
  ns->base.iterate_close = fault_iterate_close;
  *out_namespace = &ns->base;
  return H2_PAL_OK;
}

static void fault_pref_init(fault_pref_t *fault,
                            const h2_pal_pref_api_t *inner) {
  memset(fault, 0, sizeof(*fault));
  fault->inner = inner;
  fault->vtable.open = fault_open;
  fault->api.user = fault;
  fault->api.vtable = &fault->vtable;
}

int main(void) {
  char root[128];
  char database[160];
  h2_pal_e2e_config_t config = {.suite_mask = H2_PAL_E2E_SUITE_PREF};
  unsigned pass;
  snprintf(root, sizeof(root), "/tmp/h2-pal-pref-e2e-%ld", (long)getpid());
  assert(mkdir(root, 0700) == 0);
  snprintf(database, sizeof(database), "%s/prefs.sqlite", root);
  {
    h2_sqlite_t *provider = NULL;
    h2_runtime_t runtime;
    h2_pal_e2e_result_t result;
    fault_pref_t fault;
    const h2_sqlite_config_t sqlite_config = {.path = database};
    memset(&runtime, 0, sizeof(runtime));
    assert(h2_sqlite_create(&sqlite_config, &provider) == H2_PAL_OK);
    fault_pref_init(&fault, h2_sqlite_pref_api(provider));
    fault.fail_set_blob_once = 1;
    runtime.mem = &s_mem;
    runtime.pref = &fault.api;
    assert(h2_pal_e2e_run(&runtime, &config, &result) == H2_PAL_ERR_IO);
    assert(result.failed >= 2u);
    assert(result.action == H2_PAL_E2E_ACTION_NONE);
    assert(result.pref_phase == H2_PAL_E2E_PREF_PHASE_SEED);
    assert(fault.close_count == 2u);
    h2_sqlite_destroy(provider);
    assert(unlink(database) == 0);
  }
  for (pass = 0u; pass < 4u; ++pass) {
    h2_sqlite_t *provider = NULL;
    h2_runtime_t runtime;
    h2_pal_e2e_result_t result;
    const h2_sqlite_config_t sqlite_config = {.path = database};
    memset(&runtime, 0, sizeof(runtime));
    assert(h2_sqlite_create(&sqlite_config, &provider) == H2_PAL_OK);
    runtime.mem = &s_mem;
    runtime.pref = h2_sqlite_pref_api(provider);
    int rc = h2_pal_e2e_run(&runtime, &config, &result);
    if (rc != H2_PAL_OK) {
      fprintf(stderr, "pass=%u rc=%d phase=%d selected=%zu passed=%zu failed=%zu\n",
              pass, rc, result.pref_phase, result.selected, result.passed,
              result.failed);
    }
    assert(rc == H2_PAL_OK);
    assert(result.complete == 1 && result.failed == 0u);
    if (pass < 2u) assert(result.action == H2_PAL_E2E_ACTION_REBOOT);
    else assert(result.action == H2_PAL_E2E_ACTION_NONE);
    if (pass == 3u) assert(result.pref_phase == H2_PAL_E2E_PREF_PHASE_COMPLETE);
    h2_sqlite_destroy(provider);
  }
  assert(unlink(database) == 0);
  assert(rmdir(root) == 0);
  return 0;
}
