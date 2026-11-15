#include "h2_alipay_tiga_storage.h"

#include "h2_alipay_tiga_identity.h"
#include "h2_memory_pref.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t* s_runtime;
static h2_memory_pref_t* s_memory;
static h2_pal_pref_api_t s_pref;
static const char* const legacy[] = {
    "alipay_psa",    "alipay_sn_psa",   "alipay_h106_psa", "h106_pay_psa",
    "alipay_direct", "alipay_h106_ctx", "h106_pay_ctx",
};

h2_alipay_storage_scope_t h2_alipay_tiga_storage_scope(void) {
#ifdef H2_ALIPAY_E2E_PERSISTENT_STORAGE
  return H2_ALIPAY_STORAGE_PERSISTENT;
#else
  return H2_ALIPAY_STORAGE_SESSION;
#endif
}

static int close_ns(h2_pal_pref_namespace_t* ns, int rc) {
  int closed = ns->close != NULL ? ns->close(ns) : H2_PAL_ERR_UNSUPPORTED;
  rc = rc != H2_PAL_OK ? rc : closed;
  return rc <= 0 ? rc : H2_PAL_ERR_IO;
}

static int count_entries(const h2_pal_pref_api_t* pref, const char* name,
                         size_t* count) {
  *count = 0u;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(pref, name, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;
  if (rc != H2_PAL_OK) return rc;
  if (ns == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (ns->iterate == NULL || ns->iterate_close == NULL)
    return close_ns(ns, H2_PAL_ERR_UNSUPPORTED);
  h2_pal_pref_cursor_t* cursor = NULL;
  h2_pal_pref_entry_t entry;
  while ((rc = ns->iterate(ns, &cursor, &entry)) == H2_PAL_OK) ++*count;
  if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  int closed = ns->iterate_close(ns, &cursor);
  if (rc == H2_PAL_OK) rc = closed;
  return close_ns(ns, rc);
}

static int purge_history(h2_runtime_t* runtime) {
  for (size_t i = 0u; i < sizeof(legacy) / sizeof(*legacy); ++i) {
    size_t before = 0u, after = 0u;
    int rc = count_entries(runtime->pref, legacy[i], &before);
    if (rc == H2_PAL_OK && before != 0u) {
      h2_pal_pref_namespace_t* ns = NULL;
      rc = h2_pal_pref_open(runtime->pref, legacy[i],
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
      if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
      if (rc == H2_PAL_OK) {
        rc = ns->clear != NULL && ns->commit != NULL ? ns->clear(ns)
                                                     : H2_PAL_ERR_UNSUPPORTED;
        if (rc == H2_PAL_OK) rc = ns->commit(ns);
        rc = close_ns(ns, rc);
      }
    }
    if (rc == H2_PAL_OK) rc = count_entries(runtime->pref, legacy[i], &after);
    if (rc == H2_PAL_OK && after != 0u) rc = H2_PAL_ERR_INVALID_STATE;
    printf("H2_ALIPAY_STORAGE_CLEAN namespace=%s before=%zu after=%zu rc=%d\n",
           legacy[i], before, after, rc);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

static int open_routed(void* user, const char* name,
                       h2_pal_pref_open_mode_t mode,
                       h2_pal_pref_namespace_t** out) {
  (void)user;
  if (out == NULL || name == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (s_runtime == NULL) return H2_PAL_ERR_INVALID_STATE;
  const h2_pal_pref_api_t* pref = NULL;
  if (strcmp(name, "h106_pay_psa") == 0 || strcmp(name, "h106_pay_ctx") == 0)
    pref = h2_memory_pref_api(s_memory);
  else if (strcmp(name, "h106_pay_cert") == 0)
    pref = s_runtime->pref;
  else {
    *out = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return h2_pal_pref_open(pref, name, mode, out);
}

h2_pal_result_t h2_alipay_tiga_storage_configure(
    h2_runtime_t* runtime, const h2_pal_pref_api_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (runtime == NULL || runtime->mem == NULL || runtime->pref == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_runtime != NULL) {
    if (s_runtime != runtime) return H2_PAL_ERR_INVALID_STATE;
    *out = h2_alipay_tiga_storage_scope() == H2_ALIPAY_STORAGE_SESSION
               ? &s_pref
               : runtime->pref;
    return H2_PAL_OK;
  }
  if (h2_alipay_tiga_storage_scope() == H2_ALIPAY_STORAGE_PERSISTENT) {
    s_runtime = runtime;
    *out = runtime->pref;
    puts("H2_ALIPAY_STORAGE mode=persistent");
    return H2_PAL_OK;
  }
  char before[49] = {0}, after[49] = {0};
  int rc = h2_alipay_tiga_read_sn(runtime, before, sizeof(before));
  if (rc == H2_PAL_OK) rc = purge_history(runtime);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_tiga_read_sn(runtime, after, sizeof(after));
  bool preserved = rc == H2_PAL_OK && strcmp(before, after) == 0;
  if (rc == H2_PAL_OK && !preserved) rc = H2_PAL_ERR_INVALID_STATE;
  volatile char* b = before;
  volatile char* a = after;
  for (size_t i = 0u; i < sizeof(before); ++i) b[i] = a[i] = 0;
  printf("H2_ALIPAY_STORAGE_IDENTITY preserved=%d rc=%d\n", preserved, rc);
  if (rc != H2_PAL_OK) return rc;
  const h2_memory_pref_config_t config = {
      .mem = runtime->mem,
      .max_namespaces = 2u,
      .max_entries_per_namespace = 32u,
      .max_value_bytes = 16384u,
      .max_handles = 8u,
  };
  rc = h2_memory_pref_create(&config, &s_memory);
  if (rc != H2_PAL_OK) return rc;
  static const h2_pal_pref_vtable_t vtable = {.open = open_routed};
  s_runtime = runtime;
  s_pref = (h2_pal_pref_api_t){.vtable = &vtable};
  *out = &s_pref;
  puts("H2_ALIPAY_STORAGE mode=session initial_entries=0 survives_reset=0");
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tiga_storage_audit(void) {
  if (s_runtime == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (h2_alipay_tiga_storage_scope() != H2_ALIPAY_STORAGE_SESSION)
    return H2_PAL_OK;
  size_t persistent = 0u, session = 0u;
  int rc = H2_PAL_OK;
  for (size_t i = 0u; rc == H2_PAL_OK && i < sizeof(legacy) / sizeof(*legacy);
       ++i) {
    size_t count = 0u;
    rc = count_entries(s_runtime->pref, legacy[i], &count);
    persistent += count;
  }
  const char* names[] = {"h106_pay_psa", "h106_pay_ctx"};
  for (size_t i = 0u; rc == H2_PAL_OK && i < 2u; ++i) {
    size_t count = 0u;
    rc = count_entries(h2_memory_pref_api(s_memory), names[i], &count);
    session += count;
  }
  if (rc == H2_PAL_OK && persistent != 0u) rc = H2_PAL_ERR_INVALID_STATE;
  printf(
      "H2_ALIPAY_STORAGE_AUDIT mode=session persistent_entries=%zu session_entries=%zu rc=%d\n",
      persistent, session, rc);
  return rc;
}

h2_pal_result_t h2_alipay_tiga_storage_release(void) {
  int rc = h2_memory_pref_destroy(&s_memory);
  if (rc != H2_PAL_OK) return rc;
  s_runtime = NULL;
  memset(&s_pref, 0, sizeof(s_pref));
  return H2_PAL_OK;
}
