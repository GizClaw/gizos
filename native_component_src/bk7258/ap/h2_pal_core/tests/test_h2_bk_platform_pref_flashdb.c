#include "easyflash.h"
#include "flashdb.h"
#include "h2_bk_platform_core.h"
#include "os/os.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fault only the SDK boundary; all PAL methods below are production code.
 * FlashDB can report failure either before a single-key commit or after it. */
static struct {
  char key[FDB_KV_NAME_MAX];
  void *data;
  size_t size;
} entries[64];
static size_t write_count;
static char write_keys[256][FDB_KV_NAME_MAX];
static int fail_metadata, fail_value, fail_after_commit, fail_reads, fail_alloc;
static fdb_err_t write_error = FDB_WRITE_ERR;
static char legacy_key[FDB_KV_NAME_MAX];
static unsigned char legacy_data[64];
static size_t legacy_size;
static int fail_legacy_delete;
static unsigned mutexes[4], mutex_count;
static unsigned diagnostic_records;

/* Diagnostics may block: require every real provider mutex to be released. */
int h2_test_pref_printf(const char *format, ...) {
  (void)format;
  for (unsigned i = 0; i < mutex_count; ++i)
    assert(mutexes[i] == 0u);
  ++diagnostic_records;
  return 0;
}

static h2_pal_result_t diagnostic_now(void *user, uint64_t *out) {
  static uint64_t now;
  (void)user;
  *out = (now += 1000u); /* Exercise each slow-call diagnostic boundary. */
  return H2_PAL_OK;
}
static int find(const char *key) {
  for (unsigned i = 0; i < 64; ++i)
    if (entries[i].data && !strcmp(entries[i].key, key))
      return (int)i;
  return -1;
}
static void store(const char *key, const void *data, size_t size) {
  int index = find(key);
  if (index < 0) {
    for (unsigned i = 0; i < 64; ++i)
      if (!entries[i].data) {
        index = (int)i;
        break;
      }
  }
  assert(index >= 0 && size && strlen(key) < FDB_KV_NAME_MAX);
  void *copy = malloc(size);
  assert(copy);
  memcpy(copy, data, size);
  free(entries[index].data);
  entries[index].data = copy;
  entries[index].size = size;
  strcpy(entries[index].key, key);
}
void fdb_kvdb_control(fdb_kvdb_t db, int command, void *value) {
  (void)db;
  (void)command;
  (void)value;
}
fdb_err_t fdb_kvdb_init(fdb_kvdb_t db, const char *name, const char *path,
                        void *defaults, void *user) {
  (void)name;
  (void)path;
  (void)defaults;
  db->parent.user_data = user;
  return FDB_NO_ERR;
}
fdb_blob_t fdb_blob_make(fdb_blob_t blob, const void *data, size_t size) {
  blob->buf = (void *)data;
  blob->size = size;
  return blob;
}
struct fdb_kv *fdb_kv_get_obj(fdb_kvdb_t db, const char *key,
                              struct fdb_kv *out) {
  (void)db;
  int i = find(key);
  if (i < 0)
    return NULL;
  strcpy(out->name, key);
  out->value_len = entries[i].size;
  return out;
}
size_t fdb_kv_get_blob(fdb_kvdb_t db, const char *key, fdb_blob_t blob) {
  (void)db;
  int i = find(key);
  if (i < 0 || fail_reads)
    return 0;
  size_t n = entries[i].size < blob->size ? entries[i].size : blob->size;
  memcpy(blob->buf, entries[i].data, n);
  return n;
}
fdb_err_t fdb_kv_set_blob(fdb_kvdb_t db, const char *key, fdb_blob_t blob) {
  (void)db;
  assert(write_count < 256);
  strcpy(write_keys[write_count++], key);
  int metadata = !strncmp(key, "$h2t.", 5);
  int fail = metadata ? fail_metadata : fail_value;
  if (!fail || fail_after_commit)
    store(key, blob->buf, blob->size);
  return fail ? write_error : FDB_NO_ERR;
}
fdb_err_t fdb_kv_del(fdb_kvdb_t db, const char *key) {
  (void)db;
  int i = find(key);
  if (i >= 0) {
    free(entries[i].data);
    entries[i].data = NULL;
  }
  return FDB_NO_ERR;
}
void fdb_kv_iterator_init(struct fdb_kv_iterator *iterator) {
  memset(iterator, 0, sizeof(*iterator));
}
bool fdb_kv_iterate(fdb_kvdb_t db, struct fdb_kv_iterator *iterator) {
  for (; iterator->index < 64;) {
    unsigned i = (unsigned)iterator->index++;
    if (entries[i].data) {
      fdb_kv_get_obj(db, entries[i].key, &iterator->curr_kv);
      return true;
    }
  }
  return false;
}
int easyflash_init(void) { return EF_NO_ERR; }
size_t ef_get_env_blob(const char *key, void *data, size_t size,
                       size_t *saved) {
  size_t n = legacy_size && !strcmp(key, legacy_key) ? legacy_size : 0;
  if (saved)
    *saved = n;
  if (data) {
    if (n > size)
      n = size;
    memcpy(data, legacy_data, n);
  }
  return n;
}
int ef_del_env(const char *key) {
  if (fail_legacy_delete)
    return -1;
  if (!strcmp(key, legacy_key))
    legacy_size = 0;
  return EF_NO_ERR;
}
const h2_pal_time_api_t *h2_bk_platform_time_api(void) {
  static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms = diagnostic_now};
  static const h2_pal_time_api_t api = {.vtable = &vtable};
  return &api;
}
int rtos_init_mutex(beken_mutex_t *mutex) {
  assert(mutex_count < 4);
  *mutex = &mutexes[mutex_count++];
  return 0;
}
int rtos_deinit_mutex(beken_mutex_t *mutex) {
  *mutex = NULL;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t *mutex) {
  unsigned *state = *mutex;
  assert(state && !*state);
  *state = 1;
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t *mutex) {
  unsigned *state = *mutex;
  assert(state && *state);
  *state = 0;
  return 0;
}
void *os_malloc(size_t size) { return fail_alloc ? NULL : malloc(size); }
void *os_zalloc(size_t size) { return fail_alloc ? NULL : calloc(1, size); }
void *os_realloc(void *p, size_t size) {
  return fail_alloc ? NULL : realloc(p, size);
}
void os_free(void *p) { free(p); }
static void *allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void release(void *user, void *p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t mem_methods = {.alloc = allocate,
                                                .free = release};
static const h2_pal_mem_api_t mem = {.vtable = &mem_methods};
static h2_pal_pref_namespace_t *open_namespace(const char *name) {
  h2_pal_pref_namespace_t *ns = NULL;
  assert(h2_pal_pref_open(h2_bk_platform_pref_api(), name,
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns);
  return ns;
}
static void expect_type(h2_pal_pref_namespace_t *ns, const char *key,
                        h2_pal_pref_entry_type_t type) {
  h2_pal_pref_cursor_t *cursor = NULL;
  h2_pal_pref_entry_t entry;
  int found = 0, rc;
  while ((rc = ns->iterate(ns, &cursor, &entry)) == 0) {
    if (!strcmp(entry.key, key)) {
      assert(entry.type == type);
      ++found;
    }
    assert(strncmp(entry.key, "$h2t", 4));
  }
  assert(rc == H2_PAL_ERR_NOT_FOUND && found == 1);
  assert(ns->iterate_close(ns, &cursor) == 0 && !cursor);
}
static void expect_blob(h2_pal_pref_namespace_t *ns, const char *key,
                        const void *expected, size_t size) {
  void *out = NULL;
  size_t length = 0;
  assert(ns->get_blob(ns, &mem, key, &out, &length) == 0);
  assert(length == size && !memcmp(out, expected, size));
  free(out);
  expect_type(ns, key, H2_PAL_PREF_ENTRY_BLOB);
}
static void reset_faults(void) {
  fail_metadata = fail_value = fail_after_commit = fail_reads = fail_alloc = 0;
  fail_legacy_delete = 0;
  write_count = 0;
  write_error = FDB_WRITE_ERR;
}
static void reserved_namespace(void) {
  const char *names[] = {"$h2t", "$h2t.child"};
  for (unsigned i = 0; i < 2; ++i)
    for (int mode = 0; mode < 2; ++mode) {
      h2_pal_pref_namespace_t *ns = (void *)(uintptr_t)1;
      assert(h2_pal_pref_open(h2_bk_platform_pref_api(), names[i],
                              mode ? H2_PAL_PREF_OPEN_READ_WRITE
                                   : H2_PAL_PREF_OPEN_READ_ONLY,
                              &ns) == H2_PAL_ERR_INVALID_ARG);
      assert(!ns);
    }
  h2_pal_pref_namespace_t *near = open_namespace("$h2tx");
  assert(near->set_bool(near, "ordinary", 1) == 0);
  assert(near->clear(near) == 0 && near->close(near) == 0);
}
static void write_failures(void) {
  const uint8_t original[] = {9, 8, 7, 6};
  for (int after = 0; after < 2; ++after)
    for (int metadata = 0; metadata < 2; ++metadata) {
      reset_faults();
      h2_pal_pref_namespace_t *ns = open_namespace("failure");
      assert(ns->set_blob(ns, "value", original, sizeof(original)) == 0);
      reset_faults();
      fail_metadata = metadata;
      fail_value = !metadata;
      fail_after_commit = after;
      assert(ns->set_u32(ns, "value", 12345) == H2_PAL_ERR_IO);
      /* No fallible compensating writes; metadata always precedes value. */
      assert(write_count == (metadata ? 1u : 2u));
      assert(!strncmp(write_keys[0], "$h2t.", 5));
      if (!metadata)
        assert(!strcmp(write_keys[1], "failure.value"));
      reset_faults();
      assert(ns->close(ns) == 0);
      ns = open_namespace("failure");
      if (!metadata && after) {
        uint32_t value = 0;
        assert(ns->get_u32(ns, "value", &value) == 0 && value == 12345);
        expect_type(ns, "value", H2_PAL_PREF_ENTRY_U32);
      } else {
        expect_blob(ns, "value", original, sizeof(original));
        uint32_t value;
        assert(ns->get_u32(ns, "value", &value) == H2_PAL_ERR_INVALID_STATE);
      }
      assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
    }
}
static void new_value_failure(void) {
  h2_pal_pref_namespace_t *ns = open_namespace("new");
  for (int metadata = 0; metadata < 2; ++metadata) {
    reset_faults();
    fail_metadata = metadata;
    fail_value = !metadata;
    write_error = FDB_SAVED_FULL;
    assert(ns->set_string(ns, "missing", "value") == H2_PAL_ERR_NO_SPACE);
    reset_faults();
    char *value = NULL;
    assert(ns->get_string(ns, &mem, "missing", &value) ==
               H2_PAL_ERR_NOT_FOUND &&
           !value);
  }
  assert(ns->set_string(ns, "missing", "retry") == 0);
  expect_type(ns, "missing", H2_PAL_PREF_ENTRY_STRING);
  assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
}
static void identical_bytes_and_types(void) {
  h2_pal_pref_namespace_t *ns = open_namespace("same");
  uint32_t value = 27;
  assert(ns->set_blob(ns, "key", &value, sizeof(value)) == 0);
  reset_faults();
  fail_value = 1;
  assert(ns->set_u32(ns, "key", value) == 0); /* only metadata is committed */
  assert(write_count == 1 && !strncmp(write_keys[0], "$h2t.", 5));
  reset_faults();
  expect_type(ns, "key", H2_PAL_PREF_ENTRY_U32);
  assert(ns->set_i32(ns, "signed", -12) == 0);
  int32_t signed_value;
  assert(ns->get_i32(ns, "signed", &signed_value) == 0 && signed_value == -12);
  assert(ns->set_bool(ns, "boolean", 2) == 0);
  int boolean;
  assert(ns->get_bool(ns, "boolean", &boolean) == 0 && boolean == 1);
  assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
}
static void read_and_allocation_failures(void) {
  h2_pal_pref_namespace_t *ns = open_namespace("read");
  assert(ns->set_u32(ns, "key", 42) == 0);
  reset_faults();
  fail_alloc = 1;
  assert(ns->set_string(ns, "key", "changed") == H2_PAL_ERR_NO_MEMORY &&
         write_count == 0);
  reset_faults();
  fail_reads = 1;
  assert(ns->set_string(ns, "key", "changed") == H2_PAL_ERR_IO &&
         write_count == 0);
  reset_faults();
  uint32_t value;
  assert(ns->get_u32(ns, "key", &value) == 0 && value == 42);
  assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
}
static void legacy_update(void) {
  h2_pal_pref_namespace_t *ns = open_namespace("legacy");
  strcpy(legacy_key, "legacy.key");
  memcpy(legacy_data, "old!", 4);
  legacy_size = 4;
  reset_faults();
  fail_legacy_delete = 1;
  assert(ns->set_string(ns, "key", "new") == H2_PAL_ERR_IO);
  reset_faults();
  char *value = NULL;
  assert(ns->get_string(ns, &mem, "key", &value) == 0 &&
         !strcmp(value, "old!"));
  free(value);
  assert(ns->set_string(ns, "key", "new") == 0);
  expect_type(ns, "key", H2_PAL_PREF_ENTRY_STRING);
  assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
}
static void v1_and_legacy_compatibility(void) {
  /* Golden H2TYPE1 record: key identity, U32, 4 raw little-endian bytes. */
  uint8_t record[88] = {0x48, 0x32, 0x54, 0x59, 0x50, 0x45, 0x31, 0x03,
                        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x65, 0x33, 0x7c, 0xe4, 0x53, 0xd0, 0xcf, 0xcc};
  memcpy(record + 24, "compat.key", sizeof("compat.key"));
  const uint8_t bytes[] = {0x78, 0x56, 0x34, 0x12};
  store("compat.key", bytes, sizeof(bytes));
  store("$h2t.b9a1a25d2840b83a", record, sizeof(record));
  h2_pal_pref_namespace_t *ns = open_namespace("compat");
  uint32_t value = 0;
  assert(ns->get_u32(ns, "key", &value) == 0 && value == 0x12345678u);
  expect_type(ns, "key", H2_PAL_PREF_ENTRY_U32);
  reset_faults();
  fail_value = 1;
  const uint8_t replacement[] = {0x78, 0x56, 0x34, 0x22};
  assert(ns->set_blob(ns, "key", replacement, sizeof(replacement)) ==
         H2_PAL_ERR_IO);
  reset_faults();
  assert(ns->get_u32(ns, "key", &value) == 0 && value == 0x12345678u);
  expect_type(ns, "key", H2_PAL_PREF_ENTRY_U32);
  assert(ns->set_blob(ns, "key", replacement, sizeof(replacement)) == 0);
  expect_blob(ns, "key", replacement, sizeof(replacement));
  /* An older Loader may subsequently write unrelated raw bytes. */
  store("compat.key", "old firmware", 12);
  char *string = NULL;
  assert(ns->get_string(ns, &mem, "key", &string) == 0 &&
         !strcmp(string, "old firmware"));
  free(string);
  expect_type(ns, "key", H2_PAL_PREF_ENTRY_UNKNOWN);
  assert(ns->clear(ns) == 0 && ns->close(ns) == 0);
}

static void failed_creation_cleanup(void) {
  h2_pal_pref_namespace_t *ns = open_namespace("orphan");
  h2_pal_pref_namespace_t *other = open_namespace("other");
  assert(other->set_u32(other, "keep", 73) == 0);
  reset_faults();
  fail_value = 1;
  assert(ns->set_string(ns, "failed", "payload") == H2_PAL_ERR_IO);
  char metadata[FDB_KV_NAME_MAX];
  strcpy(metadata, write_keys[0]);
  reset_faults();
  h2_pal_pref_cursor_t *cursor = NULL;
  h2_pal_pref_entry_t entry;
  assert(ns->iterate(ns, &cursor, &entry) == H2_PAL_ERR_NOT_FOUND);
  assert(ns->iterate_close(ns, &cursor) == 0);
  assert(find(metadata) >= 0 && find("orphan.failed") < 0);
  assert(ns->clear(ns) == 0 && find(metadata) < 0);
  expect_type(other, "keep", H2_PAL_PREF_ENTRY_U32);
  reset_faults();
  fail_value = 1;
  assert(ns->set_bool(ns, "failed", 1) == H2_PAL_ERR_IO);
  strcpy(metadata, write_keys[0]);
  reset_faults();
  assert(ns->remove(ns, "failed") == H2_PAL_ERR_NOT_FOUND &&
         find(metadata) < 0);
  assert(other->clear(other) == 0 && other->close(other) == 0);
  assert(ns->close(ns) == 0);
}

static void simultaneous_handles(void) {
  h2_pal_pref_namespace_t *handles[8] = {0};
  for (size_t i = 0; i < 8; ++i) {
    assert(h2_pal_pref_open(h2_bk_platform_pref_api(), "parallel",
                            i % 2 ? H2_PAL_PREF_OPEN_READ_ONLY
                                  : H2_PAL_PREF_OPEN_READ_WRITE,
                            &handles[i]) == H2_PAL_OK);
    assert(handles[i]);
    for (size_t j = 0; j < i; ++j)
      assert(handles[i] != handles[j]);
  }
  assert(handles[0]->set_u32(handles[0], "value", 42u) == H2_PAL_OK);
  assert(handles[0]->commit(handles[0]) == H2_PAL_OK);
  h2_pal_pref_namespace_t *failed = (void *)(uintptr_t)1;
  fail_alloc = 1;
  assert(h2_pal_pref_open(h2_bk_platform_pref_api(), "parallel",
                          H2_PAL_PREF_OPEN_READ_ONLY, &failed) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(!failed);
  fail_alloc = 0;
  for (size_t i = 0; i < 8; ++i) {
    uint32_t value = 0;
    assert(handles[i]->get_u32(handles[i], "value", &value) == H2_PAL_OK &&
           value == 42u);
    assert(handles[i]->close(handles[i]) == H2_PAL_OK);
  }
  h2_pal_pref_namespace_t *reopened = open_namespace("parallel");
  assert(reopened->clear(reopened) == H2_PAL_OK);
  assert(reopened->close(reopened) == H2_PAL_OK);
}

int main(void) {
  reserved_namespace();
  reset_faults();
  write_failures();
  reset_faults();
  new_value_failure();
  reset_faults();
  identical_bytes_and_types();
  reset_faults();
  read_and_allocation_failures();
  reset_faults();
  legacy_update();
  reset_faults();
  v1_and_legacy_compatibility();
  reset_faults();
  failed_creation_cleanup();
  reset_faults();
  simultaneous_handles();
  for (unsigned i = 0; i < 64; ++i)
    free(entries[i].data);
  for (unsigned i = 0; i < mutex_count; ++i)
    assert(mutexes[i] == 0);
  assert(diagnostic_records != 0u);
  puts("BK Preferences: namespace isolation and metadata/value failure "
       "consistency PASS");
  return 0;
}
