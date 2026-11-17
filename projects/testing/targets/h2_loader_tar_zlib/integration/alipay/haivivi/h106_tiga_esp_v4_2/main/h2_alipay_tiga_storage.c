#include "h2_alipay_tiga_storage.h"

#include "h2_alipay_tiga_identity.h"
#include <stdio.h>
#include <string.h>

#define SESSION_ENTRIES 32u
#define SESSION_KEY_BYTES 32u
#define SESSION_VALUE_BYTES 16384u

typedef struct session_entry {
  char key[SESSION_KEY_BYTES];
  uint8_t* data;
  size_t size;
  h2_pal_pref_entry_type_t type;
  bool used;
} session_entry_t;

typedef struct session_space {
  const char* name;
  session_entry_t entries[SESSION_ENTRIES];
  h2_pal_pref_namespace_t reader, writer;
} session_space_t;

static h2_runtime_t* s_runtime;
static h2_pal_pref_api_t s_pref;
static session_space_t s_session[] = {
    {.name = "h106_pay_psa"},
    {.name = "h106_pay_ctx"},
};
static size_t s_session_handles;
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

static void session_drop(session_entry_t* entry) {
  if (entry->data != NULL) {
    volatile uint8_t* bytes = entry->data;
    for (size_t index = 0u; index < entry->size; ++index) bytes[index] = 0u;
    h2_pal_mem_free(s_runtime->mem, entry->data);
  }
  memset(entry, 0, sizeof(*entry));
}

static void session_drop_all(session_space_t* space) {
  for (size_t index = 0u; index < SESSION_ENTRIES; ++index)
    if (space->entries[index].used) session_drop(&space->entries[index]);
}

static bool session_key_valid(const char* key) {
  if (key == NULL || key[0] == '\0') return false;
  for (size_t index = 0u; index < SESSION_KEY_BYTES; ++index)
    if (key[index] == '\0') return true;
  return false;
}

static session_entry_t* session_find(session_space_t* space, const char* key) {
  for (size_t index = 0u; index < SESSION_ENTRIES; ++index)
    if (space->entries[index].used &&
        strcmp(space->entries[index].key, key) == 0)
      return &space->entries[index];
  return NULL;
}

static int session_writable(h2_pal_pref_namespace_t* ns,
                            session_space_t** space) {
  *space = ns != NULL ? ns->user : NULL;
  if (*space == NULL) return H2_PAL_ERR_INVALID_ARG;
  return ns == &(*space)->writer ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static int session_get_blob(h2_pal_pref_namespace_t* ns,
                            const h2_pal_mem_api_t* mem, const char* key,
                            void** out, size_t* size) {
  if (out == NULL || size == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  *size = 0u;
  session_space_t* space = ns != NULL ? ns->user : NULL;
  if (space == NULL || mem == NULL || !session_key_valid(key))
    return H2_PAL_ERR_INVALID_ARG;
  const session_entry_t* entry = session_find(space, key);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (entry->type != H2_PAL_PREF_ENTRY_BLOB) return H2_PAL_ERR_FORMAT;
  void* copy = h2_pal_mem_alloc(mem, entry->size != 0u ? entry->size : 1u);
  if (copy == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (entry->size != 0u) memcpy(copy, entry->data, entry->size);
  *out = copy;
  *size = entry->size;
  return H2_PAL_OK;
}

static int session_set_value(h2_pal_pref_namespace_t* ns, const char* key,
                             const void* data, size_t size,
                             h2_pal_pref_entry_type_t type) {
  session_space_t* space = NULL;
  int rc = session_writable(ns, &space);
  if (rc != H2_PAL_OK) return rc;
  if (!session_key_valid(key) || (data == NULL && size != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (size > SESSION_VALUE_BYTES) return H2_PAL_ERR_NO_SPACE;
  session_entry_t* entry = session_find(space, key);
  for (size_t index = 0u; entry == NULL && index < SESSION_ENTRIES; ++index)
    if (!space->entries[index].used) entry = &space->entries[index];
  if (entry == NULL) return H2_PAL_ERR_NO_SPACE;
  uint8_t* copy = NULL;
  if (size != 0u) {
    copy = h2_pal_mem_alloc(s_runtime->mem, size);
    if (copy == NULL) return H2_PAL_ERR_NO_MEMORY;
    memcpy(copy, data, size);
  }
  session_drop(entry);
  memcpy(entry->key, key, strlen(key) + 1u);
  entry->data = copy;
  entry->size = size;
  entry->type = type;
  entry->used = true;
  return H2_PAL_OK;
}

static int session_set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                            const void* data, size_t size) {
  return session_set_value(ns, key, data, size, H2_PAL_PREF_ENTRY_BLOB);
}

static int session_get_u32(h2_pal_pref_namespace_t* ns, const char* key,
                           uint32_t* out) {
  session_space_t* space = ns != NULL ? ns->user : NULL;
  if (space == NULL || out == NULL || !session_key_valid(key))
    return H2_PAL_ERR_INVALID_ARG;
  const session_entry_t* entry = session_find(space, key);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (entry->type != H2_PAL_PREF_ENTRY_U32 || entry->size != 4u ||
      entry->data == NULL)
    return H2_PAL_ERR_FORMAT;
  uint32_t value = 0u;
  for (size_t index = 0u; index < 4u; ++index)
    value |= (uint32_t)entry->data[index] << (index * 8u);
  *out = value;
  return H2_PAL_OK;
}

static int session_set_u32(h2_pal_pref_namespace_t* ns, const char* key,
                           uint32_t value) {
  uint8_t bytes[4];
  for (size_t index = 0u; index < sizeof(bytes); ++index)
    bytes[index] = (uint8_t)(value >> (index * 8u));
  return session_set_value(ns, key, bytes, sizeof(bytes),
                           H2_PAL_PREF_ENTRY_U32);
}

static int session_remove(h2_pal_pref_namespace_t* ns, const char* key) {
  session_space_t* space = NULL;
  int rc = session_writable(ns, &space);
  if (rc != H2_PAL_OK) return rc;
  if (!session_key_valid(key)) return H2_PAL_ERR_INVALID_ARG;
  session_entry_t* entry = session_find(space, key);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  session_drop(entry);
  return H2_PAL_OK;
}

static int session_clear(h2_pal_pref_namespace_t* ns) {
  session_space_t* space = NULL;
  int rc = session_writable(ns, &space);
  if (rc == H2_PAL_OK) session_drop_all(space);
  return rc;
}

static int session_commit(h2_pal_pref_namespace_t* ns) {
  session_space_t* space = NULL;
  return session_writable(ns, &space);
}

static int session_close(h2_pal_pref_namespace_t* ns) {
  if (ns == NULL || ns->user == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_session_handles == 0u) return H2_PAL_ERR_INVALID_STATE;
  ns->user = NULL;
  --s_session_handles;
  return H2_PAL_OK;
}

static int session_open(session_space_t* space, h2_pal_pref_open_mode_t mode,
                        h2_pal_pref_namespace_t** out) {
  if (mode != H2_PAL_PREF_OPEN_READ_ONLY && mode != H2_PAL_PREF_OPEN_READ_WRITE)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_pref_namespace_t* ns =
      mode == H2_PAL_PREF_OPEN_READ_WRITE ? &space->writer : &space->reader;
  if (ns->user != NULL) return H2_PAL_ERR_BUSY;
  *ns = (h2_pal_pref_namespace_t){
      .user = space,
      .close = session_close,
      .get_blob = session_get_blob,
      .set_blob = session_set_blob,
      .get_u32 = session_get_u32,
      .set_u32 = session_set_u32,
      .remove = session_remove,
      .clear = session_clear,
      .commit = session_commit,
  };
  ++s_session_handles;
  *out = ns;
  return H2_PAL_OK;
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
  for (size_t index = 0u; index < sizeof(s_session) / sizeof(*s_session);
       ++index)
    if (strcmp(name, s_session[index].name) == 0)
      return session_open(&s_session[index], mode, out);
  if (strcmp(name, "h106_pay_cert") == 0)
    return h2_pal_pref_open(s_runtime->pref, name, mode, out);
  return H2_PAL_ERR_UNSUPPORTED;
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
  for (size_t index = 0u; index < sizeof(s_session) / sizeof(*s_session);
       ++index)
    for (size_t entry_index = 0u; entry_index < SESSION_ENTRIES; ++entry_index)
      if (s_session[index].entries[entry_index].used) ++session;
  if (rc == H2_PAL_OK && persistent != 0u) rc = H2_PAL_ERR_INVALID_STATE;
  printf(
      "H2_ALIPAY_STORAGE_AUDIT mode=session persistent_entries=%zu session_entries=%zu rc=%d\n",
      persistent, session, rc);
  return rc;
}

h2_pal_result_t h2_alipay_tiga_storage_release(void) {
  if (s_session_handles != 0u) return H2_PAL_ERR_INVALID_STATE;
  if (s_runtime != NULL)
    for (size_t index = 0u; index < sizeof(s_session) / sizeof(*s_session);
         ++index)
      session_drop_all(&s_session[index]);
  s_runtime = NULL;
  memset(&s_pref, 0, sizeof(s_pref));
  return H2_PAL_OK;
}
