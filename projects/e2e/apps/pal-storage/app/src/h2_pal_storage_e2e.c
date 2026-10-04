#include "h2_pal_storage_e2e.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(v)                                                             \
  do {                                                                         \
    if (!(v))                                                                  \
      return H2_PAL_ERR_INVALID_STATE;                                         \
  } while (0)
#define CALL(v)                                                                \
  do {                                                                         \
    int rc_ = (v);                                                             \
    if (rc_ != H2_PAL_OK)                                                      \
      return (h2_pal_result_t)rc_;                                             \
  } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define LARGE_BLOB_SIZE (16u * 1024u)
#define OVERWRITE_COUNT 1000u

typedef struct owner {
  h2_runtime_t *runtime;
  h2_pal_storage_config_t config;
  h2_pal_fs_file_t *file;
  h2_pal_pref_namespace_t *a, *b;
  h2_pal_pref_cursor_t *cursor;
  h2_pal_pref_namespace_t *cursor_namespace;
  void *value;
  void *scratch;
  int close_failed;
  char path[512], other[512];
} owner_t;
static const struct {
  const char *id;
  int phase;
} cases[] = {
#define H2_PAL_STORAGE_CASE(id, phase) {id, phase},
#include "h2_pal_storage_cases.inc"
#undef H2_PAL_STORAGE_CASE
};
static h2_pal_result_t close_file(owner_t *o) {
  if (!o->file)
    return H2_PAL_OK;
  int rc = h2_pal_fs_close(o->runtime->fs, o->file);
  /* FS does not specify whether a failed close consumed its handle. Never
   * retry such a handle; the isolated host must terminate on this failure. */
  if (rc) {
    o->close_failed = 1;
    return (h2_pal_result_t)rc;
  }
  o->file = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t close_namespace(owner_t *o,
                                       h2_pal_pref_namespace_t **ns) {
  if (!*ns)
    return H2_PAL_OK;
  if (!(*ns)->close)
    return H2_PAL_ERR_UNSUPPORTED;
  int rc = (*ns)->close(*ns);
  if (rc) {
    o->close_failed = 1;
    return (h2_pal_result_t)rc;
  }
  *ns = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t release(owner_t *o) {
  if (o->close_failed)
    return H2_PAL_ERR_INVALID_STATE;
  if (o->value) {
    h2_pal_mem_free(o->runtime->mem, o->value);
    o->value = NULL;
  }
  if (o->scratch) {
    h2_pal_mem_free(o->runtime->mem, o->scratch);
    o->scratch = NULL;
  }
  if (o->cursor) {
    h2_pal_pref_namespace_t *ns = o->cursor_namespace ? o->cursor_namespace : o->a;
    CALL(ns->iterate_close(ns, &o->cursor));
  }
  o->cursor_namespace = NULL;
  CALL(close_file(o));
  CALL(close_namespace(o, &o->b));
  return close_namespace(o, &o->a);
}
static h2_pal_result_t path(owner_t *o, const char *suffix) {
  int n = snprintf(o->path, sizeof(o->path), "%s/%s", o->config.root, suffix);
  return n < 0 || (size_t)n >= sizeof(o->path) ? H2_PAL_ERR_INVALID_ARG
                                               : H2_PAL_OK;
}
static h2_pal_result_t open_file(owner_t *o, const char *name,
                                 h2_pal_fs_open_mode_t mode) {
  CALL(close_file(o));
  CALL(path(o, name));
  return (h2_pal_result_t)h2_pal_fs_open(o->runtime->fs, o->path, mode,
                                         &o->file);
}
static h2_pal_result_t write_file(owner_t *o, const char *name,
                                  const void *data, size_t length) {
  CALL(open_file(o, name, H2_PAL_FS_OPEN_WRITE_TRUNCATE));
  size_t written = 0;
  CALL(h2_pal_fs_write(o->runtime->fs, o->file, data, length, &written));
  REQUIRE(written == length);
  CALL(h2_pal_fs_sync(o->runtime->fs, o->file));
  return close_file(o);
}
static h2_pal_result_t read_file(owner_t *o, const char *name, const void *data,
                                 size_t length) {
  unsigned char readback[2048];
  size_t got = 0;
  REQUIRE(length <= sizeof(readback));
  CALL(open_file(o, name, H2_PAL_FS_OPEN_READ));
  CALL(h2_pal_fs_read(o->runtime->fs, o->file, readback, sizeof(readback),
                      &got));
  REQUIRE(got == length && memcmp(readback, data, length) == 0);
  CALL(h2_pal_fs_read(o->runtime->fs, o->file, readback, sizeof(readback),
                      &got));
  REQUIRE(got == 0);
  CALL(h2_pal_fs_read(o->runtime->fs, o->file, NULL, 0, &got));
  REQUIRE(got == 0);
  return close_file(o);
}
static h2_pal_result_t open_namespace(owner_t *o, int second,
                                      h2_pal_pref_open_mode_t mode) {
  h2_pal_pref_namespace_t **out = second ? &o->b : &o->a;
  CALL(close_namespace(o, out));
  CALL(h2_pal_pref_open(o->runtime->pref,
                        second ? o->config.namespace_b : o->config.namespace_a,
                        mode, out));
  h2_pal_pref_namespace_t *ns = *out;
  if (!ns || !ns->close || !ns->get_blob || !ns->set_blob || !ns->get_string ||
      !ns->set_string || !ns->get_u32 || !ns->set_u32 || !ns->get_i32 ||
      !ns->set_i32 || !ns->get_bool || !ns->set_bool || !ns->remove ||
      !ns->clear || !ns->commit || !ns->iterate || !ns->iterate_close)
    return H2_PAL_ERR_UNSUPPORTED;
  return H2_PAL_OK;
}
static unsigned char pattern_byte(owner_t *o, size_t index) {
  uint32_t value = (uint32_t)index ^ o->config.nonce;
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return (unsigned char)value;
}
static void pattern(owner_t *o, unsigned char *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i)
    bytes[i] = pattern_byte(o, i);
}
static void *reject_alloc(void *user, size_t length) {
  (void)user;
  (void)length;
  return NULL;
}
static const h2_pal_mem_vtable_t reject_methods = {.alloc = reject_alloc};
static const h2_pal_mem_api_t reject_memory = {.vtable = &reject_methods};
static h2_pal_result_t set_large_blob(owner_t *o) {
  o->scratch = h2_pal_mem_alloc(o->runtime->mem, LARGE_BLOB_SIZE);
  if (!o->scratch)
    return H2_PAL_ERR_NO_MEMORY;
  pattern(o, o->scratch, LARGE_BLOB_SIZE);
  CALL(o->a->set_blob(o->a, "blob", o->scratch, LARGE_BLOB_SIZE));
  h2_pal_mem_free(o->runtime->mem, o->scratch);
  o->scratch = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t seed_values(owner_t *o) {
  CALL(set_large_blob(o));
  CALL(o->a->set_string(o->a, "string",
                        "PAL storage \xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96"));
  CALL(o->a->set_u32(o->a, "u32", o->config.nonce));
  CALL(o->a->set_i32(o->a, "i32", INT32_MIN));
  return (h2_pal_result_t)o->a->set_bool(o->a, "bool", 1);
}
static h2_pal_result_t read_values(owner_t *o) {
  size_t length = 0;
  uint32_t u32 = 0;
  int32_t i32 = 0;
  int boolean = 0;
  CALL(o->a->get_blob(o->a, o->runtime->mem, "blob", &o->value, &length));
  REQUIRE(length == LARGE_BLOB_SIZE && o->value != NULL);
  for (size_t i = 0; i < length; ++i)
    REQUIRE(((unsigned char *)o->value)[i] == pattern_byte(o, i));
  h2_pal_mem_free(o->runtime->mem, o->value);
  o->value = NULL;
  CALL(o->a->get_string(o->a, o->runtime->mem, "string", (char **)&o->value));
  REQUIRE(strcmp(o->value,
                 "PAL storage \xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96") == 0);
  h2_pal_mem_free(o->runtime->mem, o->value);
  o->value = NULL;
  CALL(o->a->get_u32(o->a, "u32", &u32));
  REQUIRE(u32 == o->config.nonce);
  CALL(o->a->get_i32(o->a, "i32", &i32));
  REQUIRE(i32 == INT32_MIN);
  CALL(o->a->get_bool(o->a, "bool", &boolean));
  REQUIRE(boolean == 1);
  return H2_PAL_OK;
}
static h2_pal_result_t iteration(owner_t *o, int early, unsigned expected) {
  o->cursor_namespace = o->a;
  h2_pal_pref_entry_t entry;
  unsigned mask = 0, count = 0;
  static const char *keys[] = {"blob", "string", "u32", "i32", "bool", "counter"};
  static const h2_pal_pref_entry_type_t types[] = {
      H2_PAL_PREF_ENTRY_BLOB, H2_PAL_PREF_ENTRY_STRING, H2_PAL_PREF_ENTRY_U32,
      H2_PAL_PREF_ENTRY_I32, H2_PAL_PREF_ENTRY_BOOL, H2_PAL_PREF_ENTRY_U32};
  int rc;
  while ((rc = o->a->iterate(o->a, &o->cursor, &entry)) == H2_PAL_OK) {
    unsigned i;
    for (i = 0; i < COUNT(keys); ++i)
      if (!strcmp(entry.key, keys[i]))
        break;
    REQUIRE(i < expected && entry.type == types[i] && !(mask & (1u << i)));
    mask |= 1u << i;
    ++count;
    if (early)
      break;
  }
  if (!early)
    REQUIRE(rc == H2_PAL_ERR_NOT_FOUND && count == expected &&
            mask == (1u << expected) - 1u);
  CALL(o->a->iterate_close(o->a, &o->cursor));
  REQUIRE(o->cursor == NULL);
  CALL(o->a->iterate_close(o->a, &o->cursor));
  return H2_PAL_OK;
}
static h2_pal_result_t read_counter_isolation(owner_t *o) {
  uint32_t value = 0;
  CALL(o->a->get_u32(o->a, "counter", &value));
  REQUIRE(value == OVERWRITE_COUNT - 1u);
  CALL(iteration(o, 0, 6));
  CALL(open_namespace(o, 1, H2_PAL_PREF_OPEN_READ_ONLY));
  CALL(o->b->get_u32(o->b, "counter", &value));
  REQUIRE(value == (o->config.nonce ^ UINT32_MAX));
  h2_pal_pref_entry_t entry;
  o->cursor_namespace = o->b;
  CALL(o->b->iterate(o->b, &o->cursor, &entry));
  REQUIRE(!strcmp(entry.key, "counter") && entry.type == H2_PAL_PREF_ENTRY_U32);
  REQUIRE(o->b->iterate(o->b, &o->cursor, &entry) == H2_PAL_ERR_NOT_FOUND);
  CALL(o->b->iterate_close(o->b, &o->cursor));
  REQUIRE(o->cursor == NULL);
  return H2_PAL_OK;
}
static h2_pal_result_t run_case(owner_t *o, unsigned index) {
  h2_runtime_t *r = o->runtime;
  const h2_pal_fs_api_t *fs = r->fs;
  unsigned char bytes[1537];
  pattern(o, bytes, sizeof(bytes));
  h2_pal_fs_stat_t st = {0};
  size_t count = 0;
  uint32_t u32 = 0;
  int32_t i32 = 0;
  int boolean = 0;
  if ((index >= 11 && index <= 26) || (index >= 30 && index <= 33))
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_WRITE));
  switch (index) {
  case 0:
    CALL(h2_pal_fs_mkdir(fs, o->config.root));
    CALL(h2_pal_fs_mkdir(fs, o->config.root));
    CALL(h2_pal_fs_stat(fs, o->config.root, &st));
    REQUIRE(st.is_dir);
    break;
  case 1:
    CALL(write_file(o, "binary", bytes, sizeof(bytes)));
    CALL(read_file(o, "binary", bytes, sizeof(bytes)));
    break;
  case 2:
    CALL(write_file(o, "empty", "", 0));
    CALL(read_file(o, "empty", "", 0));
    break;
  case 3: {
    unsigned char readback[37];
    CALL(open_file(o, "binary", H2_PAL_FS_OPEN_READ));
    CALL(h2_pal_fs_seek(fs, o->file, 511));
    CALL(h2_pal_fs_read(fs, o->file, readback, sizeof(readback), &count));
    REQUIRE(count == sizeof(readback) && !memcmp(readback, bytes + 511, count));
    CALL(h2_pal_fs_seek(fs, o->file, 0));
    CALL(h2_pal_fs_read(fs, o->file, readback, sizeof(readback), &count));
    REQUIRE(count == sizeof(readback) && !memcmp(readback, bytes, count));
    break;
  }
  case 4:
    CALL(write_file(o, "binary", bytes, 7));
    CALL(path(o, "binary"));
    CALL(h2_pal_fs_stat(fs, o->path, &st));
    REQUIRE(!st.is_dir && st.size == 7);
    CALL(read_file(o, "binary", bytes, 7));
    break;
  case 5:
    CALL(path(o, "binary"));
    strcpy(o->other, o->path);
    CALL(path(o, "renamed"));
    CALL(h2_pal_fs_rename(fs, o->other, o->path));
    REQUIRE(h2_pal_fs_stat(fs, o->other, &st) == H2_PAL_ERR_NOT_FOUND);
    CALL(read_file(o, "renamed", bytes, 7));
    break;
  case 6:
    CALL(path(o, "renamed"));
    CALL(h2_pal_fs_remove(fs, o->path));
    REQUIRE(h2_pal_fs_stat(fs, o->path, &st) == H2_PAL_ERR_NOT_FOUND);
    break;
  case 7:
    CALL(path(o, "nested"));
    CALL(h2_pal_fs_mkdir(fs, o->path));
    CALL(write_file(o, "nested/data", bytes, 11));
    CALL(path(o, "nested"));
    CALL(h2_pal_fs_clear(fs, o->path));
    CALL(h2_pal_fs_stat(fs, o->path, &st));
    REQUIRE(st.is_dir);
    CALL(path(o, "nested/data"));
    REQUIRE(h2_pal_fs_stat(fs, o->path, &st) == H2_PAL_ERR_NOT_FOUND);
    break;
  case 8:
    CALL(write_file(o, "isolation-a", bytes, 13));
    CALL(write_file(o, "isolation-b", bytes + 13, 17));
    CALL(read_file(o, "isolation-a", bytes, 13));
    CALL(read_file(o, "isolation-b", bytes + 13, 17));
    break;
  case 9:
    REQUIRE(h2_pal_fs_open(fs, NULL, H2_PAL_FS_OPEN_READ, &o->file) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_read(fs, NULL, bytes, 1, &count) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_write(fs, NULL, bytes, 1, &count) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_seek(fs, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_sync(fs, NULL) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_close(fs, NULL) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_fs_stat(fs, NULL, &st) == H2_PAL_ERR_INVALID_ARG);
    break;
  case 10:
    for (unsigned i = 0; i < 100; ++i) {
      CALL(write_file(o, "churn", bytes, 31));
      CALL(read_file(o, "churn", bytes, 31));
      CALL(path(o, "churn"));
      CALL(h2_pal_fs_remove(fs, o->path));
    }
    break;
  case 11:
    CALL(o->a->clear(o->a));
    CALL(o->a->commit(o->a));
    CALL(close_namespace(o, &o->a));
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_ONLY));
    break;
  case 12:
    CALL(o->a->set_blob(o->a, "blob", bytes, sizeof(bytes)));
    CALL(o->a->get_blob(o->a, r->mem, "blob", &o->value, &count));
    REQUIRE(count == sizeof(bytes) && !memcmp(o->value, bytes, count));
    CALL(o->a->commit(o->a));
    break;
  case 13:
    CALL(o->a->set_string(o->a, "string",
                          "PAL storage \xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96"));
    CALL(o->a->get_string(o->a, r->mem, "string", (char **)&o->value));
    REQUIRE(
        !strcmp(o->value, "PAL storage \xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96"));
    CALL(o->a->commit(o->a));
    break;
  case 14:
    CALL(o->a->set_u32(o->a, "u32", UINT32_MAX));
    CALL(o->a->get_u32(o->a, "u32", &u32));
    REQUIRE(u32 == UINT32_MAX);
    CALL(o->a->commit(o->a));
    break;
  case 15:
    CALL(o->a->set_i32(o->a, "i32", INT32_MIN));
    CALL(o->a->get_i32(o->a, "i32", &i32));
    REQUIRE(i32 == INT32_MIN);
    CALL(o->a->commit(o->a));
    break;
  case 16:
    CALL(o->a->set_bool(o->a, "bool", 0));
    CALL(o->a->get_bool(o->a, "bool", &boolean));
    REQUIRE(boolean == 0);
    CALL(o->a->set_bool(o->a, "bool", 1));
    CALL(o->a->get_bool(o->a, "bool", &boolean));
    REQUIRE(boolean == 1);
    CALL(o->a->commit(o->a));
    break;
  case 17:
    CALL(o->a->set_blob(o->a, "blob", bytes, 5));
    CALL(o->a->get_blob(o->a, r->mem, "blob", &o->value, &count));
    REQUIRE(count == 5 && !memcmp(bytes, o->value, 5));
    h2_pal_mem_free(r->mem, o->value);
    o->value = NULL;
    /* PAL Pref does not prescribe one type-mismatch error code; existing
     * providers use FORMAT or INVALID_STATE. Rejection must preserve data. */
    REQUIRE(o->a->get_string(o->a, r->mem, "blob", (char **)&o->value) !=
            H2_PAL_OK);
    REQUIRE(o->value == NULL);
    CALL(o->a->get_blob(o->a, r->mem, "blob", &o->value, &count));
    REQUIRE(count == 5 && !memcmp(bytes, o->value, 5));
    h2_pal_mem_free(r->mem, o->value);
    o->value = NULL;
    CALL(seed_values(o));
    CALL(o->a->commit(o->a));
    break;
  case 18:
    CALL(open_namespace(o, 1, H2_PAL_PREF_OPEN_READ_WRITE));
    CALL(o->b->clear(o->b));
    CALL(o->b->set_u32(o->b, "u32", 123));
    CALL(o->b->commit(o->b));
    CALL(o->a->get_u32(o->a, "u32", &u32));
    REQUIRE(u32 == o->config.nonce);
    CALL(o->b->get_u32(o->b, "u32", &u32));
    REQUIRE(u32 == 123);
    break;
  case 19:
    CALL(close_namespace(o, &o->a));
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_ONLY));
    REQUIRE(o->a->set_u32(o->a, "u32", 7) == H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->set_i32(o->a, "i32", 7) == H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->set_bool(o->a, "bool", 0) == H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->set_blob(o->a, "blob", bytes, sizeof(bytes)) ==
            H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->set_string(o->a, "string", "changed") ==
            H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->remove(o->a, "u32") == H2_PAL_ERR_INVALID_STATE);
    REQUIRE(o->a->clear(o->a) == H2_PAL_ERR_INVALID_STATE);
    CALL(read_values(o));
    break;
  case 20:
    CALL(iteration(o, 0, 5));
    break;
  case 21:
    CALL(iteration(o, 1, 5));
    CALL(iteration(o, 0, 5));
    break;
  case 22:
    REQUIRE(o->a->get_blob(o->a, &reject_memory, "blob", &o->value, &count) ==
            H2_PAL_ERR_NO_MEMORY);
    REQUIRE(o->value == NULL);
    REQUIRE(o->a->get_string(o->a, &reject_memory, "string",
                             (char **)&o->value) == H2_PAL_ERR_NO_MEMORY);
    REQUIRE(o->value == NULL);
    CALL(read_values(o));
    break;
  case 23:
    CALL(o->a->remove(o->a, "u32"));
    REQUIRE(o->a->get_u32(o->a, "u32", &u32) == H2_PAL_ERR_NOT_FOUND);
    CALL(o->a->commit(o->a));
    break;
  case 24: {
    h2_pal_pref_entry_t entry;
    CALL(o->a->clear(o->a));
    CALL(o->a->commit(o->a));
    REQUIRE(o->a->iterate(o->a, &o->cursor, &entry) == H2_PAL_ERR_NOT_FOUND);
    break;
  }
  case 25:
    for (unsigned i = 0; i < 100; ++i) {
      CALL(o->a->set_u32(o->a, "counter", i));
      CALL(o->a->commit(o->a));
      CALL(close_namespace(o, &o->a));
      CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_WRITE));
      CALL(o->a->get_u32(o->a, "counter", &u32));
      REQUIRE(u32 == i);
    }
    break;
  case 26:
    CALL(write_file(o, "persistent", bytes, sizeof(bytes)));
    CALL(o->a->clear(o->a));
    CALL(seed_values(o));
    CALL(o->a->commit(o->a));
    CALL(open_namespace(o, 1, H2_PAL_PREF_OPEN_READ_WRITE));
    CALL(o->b->clear(o->b));
    CALL(o->b->set_u32(o->b, "counter", o->config.nonce ^ UINT32_MAX));
    CALL(o->b->commit(o->b));
    break;
  case 27:
    CALL(read_file(o, "persistent", bytes, sizeof(bytes)));
    break;
  case 28:
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_ONLY));
    CALL(read_values(o));
    CALL(read_counter_isolation(o));
    break;
  case 29:
    CALL(h2_pal_fs_clear(fs, o->config.root));
    CALL(h2_pal_fs_remove(fs, o->config.root));
    REQUIRE(h2_pal_fs_stat(fs, o->config.root, &st) == H2_PAL_ERR_NOT_FOUND);
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_WRITE));
    /* Keep remove and clear independent: a is removed key by key, b is
     * cleared. The next fresh provider verifies both persisted absences. */
    static const char *remove_keys[] = {"blob", "string", "u32", "i32", "bool",
                                        "counter"};
    for (unsigned i = 0; i < COUNT(remove_keys); ++i)
      CALL(o->a->remove(o->a, remove_keys[i]));
    CALL(o->a->commit(o->a));
    CALL(open_namespace(o, 1, H2_PAL_PREF_OPEN_READ_WRITE));
    CALL(o->b->clear(o->b));
    CALL(o->b->commit(o->b));
    break;
  case 30: {
    static const size_t lengths[] = {1, 255, 256, 1537, 16383, LARGE_BLOB_SIZE};
    o->scratch = h2_pal_mem_alloc(r->mem, LARGE_BLOB_SIZE);
    if (!o->scratch)
      return H2_PAL_ERR_NO_MEMORY;
    pattern(o, o->scratch, LARGE_BLOB_SIZE);
    for (unsigned i = 0; i < COUNT(lengths); ++i) {
      CALL(o->a->set_blob(o->a, "boundaryblob", o->scratch, lengths[i]));
      CALL(o->a->commit(o->a));
      CALL(close_namespace(o, &o->a));
      CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_WRITE));
      CALL(o->a->get_blob(o->a, r->mem, "boundaryblob", &o->value, &count));
      REQUIRE(count == lengths[i] && !memcmp(o->value, o->scratch, count));
      h2_pal_mem_free(r->mem, o->value);
      o->value = NULL;
    }
    CALL(o->a->remove(o->a, "boundaryblob"));
    CALL(o->a->commit(o->a));
    break;
  }
  case 31: {
    static const size_t lengths[] = {1, 255, 256, 4095};
    o->scratch = h2_pal_mem_alloc(r->mem, 4096);
    if (!o->scratch)
      return H2_PAL_ERR_NO_MEMORY;
    char *text = o->scratch;
    for (unsigned i = 0; i < COUNT(lengths); ++i) {
      for (size_t j = 0; j < lengths[i]; ++j)
        text[j] = (char)('a' + j % 26u);
      text[lengths[i]] = '\0';
      CALL(o->a->set_string(o->a, "boundarystring", text));
      CALL(o->a->commit(o->a));
      CALL(close_namespace(o, &o->a));
      CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_WRITE));
      CALL(o->a->get_string(o->a, r->mem, "boundarystring", (char **)&o->value));
      REQUIRE(strlen(o->value) == lengths[i] && !strcmp(o->value, text));
      h2_pal_mem_free(r->mem, o->value);
      o->value = NULL;
    }
    CALL(o->a->remove(o->a, "boundarystring"));
    CALL(o->a->commit(o->a));
    break;
  }
  case 32: {
    static const uint32_t unsigned_values[] = {0, 1, UINT32_MAX};
    static const int32_t signed_values[] = {INT32_MIN, -1, 0, INT32_MAX};
    for (unsigned i = 0; i < COUNT(unsigned_values); ++i) {
      CALL(o->a->set_u32(o->a, "boundaryu32", unsigned_values[i]));
      CALL(o->a->get_u32(o->a, "boundaryu32", &u32));
      REQUIRE(u32 == unsigned_values[i]);
    }
    for (unsigned i = 0; i < COUNT(signed_values); ++i) {
      CALL(o->a->set_i32(o->a, "boundaryi32", signed_values[i]));
      CALL(o->a->get_i32(o->a, "boundaryi32", &i32));
      REQUIRE(i32 == signed_values[i]);
    }
    CALL(o->a->remove(o->a, "boundaryu32"));
    CALL(o->a->remove(o->a, "boundaryi32"));
    CALL(o->a->commit(o->a));
    break;
  }
  case 33:
    /* Stress overwrite rather than handle churn; commit only the final value.
     * Providers may persist each set, so this does not assert atomicity. */
    for (unsigned i = 0; i < OVERWRITE_COUNT; ++i)
      CALL(o->a->set_u32(o->a, "counter", i));
    CALL(o->a->commit(o->a));
    CALL(close_namespace(o, &o->a));
    CALL(open_namespace(o, 0, H2_PAL_PREF_OPEN_READ_ONLY));
    CALL(read_counter_isolation(o));
    CALL(read_values(o));
    break;
  case 34:
    REQUIRE(h2_pal_fs_stat(fs, o->config.root, &st) == H2_PAL_ERR_NOT_FOUND);
    break;
  case 35: {
    h2_pal_pref_entry_t entry;
    for (unsigned second = 0; second < 2; ++second) {
      CALL(open_namespace(o, (int)second, H2_PAL_PREF_OPEN_READ_ONLY));
      h2_pal_pref_namespace_t *ns = second ? o->b : o->a;
      o->cursor_namespace = ns;
      REQUIRE(ns->iterate(ns, &o->cursor, &entry) == H2_PAL_ERR_NOT_FOUND);
      CALL(ns->iterate_close(ns, &o->cursor));
      REQUIRE(o->cursor == NULL);
      REQUIRE(ns->get_u32(ns, "counter", &u32) == H2_PAL_ERR_NOT_FOUND);
      REQUIRE(ns->get_blob(ns, r->mem, "blob", &o->value, &count) ==
              H2_PAL_ERR_NOT_FOUND);
      REQUIRE(o->value == NULL);
      REQUIRE(ns->get_string(ns, r->mem, "string", (char **)&o->value) ==
              H2_PAL_ERR_NOT_FOUND);
      REQUIRE(o->value == NULL);
      REQUIRE(ns->get_u32(ns, "u32", &u32) == H2_PAL_ERR_NOT_FOUND);
      REQUIRE(ns->get_i32(ns, "i32", &i32) == H2_PAL_ERR_NOT_FOUND);
      REQUIRE(ns->get_bool(ns, "bool", &boolean) == H2_PAL_ERR_NOT_FOUND);
    }
    break;
  }
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_pal_storage_e2e_run(h2_runtime_t *runtime,
                                       const h2_pal_storage_config_t *cfg,
                                       h2_pal_storage_result_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!runtime || !runtime->mem || !cfg || !cfg->root || !cfg->namespace_a ||
      !cfg->namespace_b || !strcmp(cfg->namespace_a, cfg->namespace_b) ||
      (cfg->phase != H2_PAL_STORAGE_SEED &&
       cfg->phase != H2_PAL_STORAGE_VERIFY &&
       cfg->phase != H2_PAL_STORAGE_CLEAN_VERIFY))
    return H2_PAL_ERR_INVALID_ARG;
  owner_t *o = h2_pal_mem_alloc(runtime->mem, sizeof(*o));
  if (!o)
    return H2_PAL_ERR_NO_MEMORY;
  memset(o, 0, sizeof(*o));
  o->runtime = runtime;
  o->config = *cfg;
  int stopped = 0;
  for (unsigned i = 0; i < COUNT(cases); ++i) {
    if (cases[i].phase != (int)cfg->phase)
      continue;
    h2_pal_result_t rc = stopped ? H2_PAL_ERR_UNAVAILABLE : run_case(o, i);
    h2_pal_result_t cleanup = stopped ? out->cleanup_result : release(o);
    if (cleanup) {
      out->cleanup_result = cleanup;
      rc = cleanup;
    }
    h2_pal_storage_status_t status =
        rc == H2_PAL_OK
            ? H2_PAL_STORAGE_PASS
            : (rc == H2_PAL_ERR_UNSUPPORTED || stopped ? H2_PAL_STORAGE_BLOCKED
                                                       : H2_PAL_STORAGE_FAIL);
    if (status == H2_PAL_STORAGE_PASS)
      ++out->passed;
    else if (status == H2_PAL_STORAGE_BLOCKED)
      ++out->blocked;
    else
      ++out->failed;
    if (cfg->case_result)
      cfg->case_result(cfg->user, cases[i].id, status, rc);
    if (rc)
      stopped = 1;
  }
  if (out->cleanup_result)
    out->retained_cleanup = o;
  else
    h2_pal_mem_free(runtime->mem, o);
  return out->failed || out->blocked || out->cleanup_result
             ? H2_PAL_ERR_INVALID_STATE
             : H2_PAL_OK;
}
