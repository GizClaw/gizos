#ifndef H2_ALIPAY_TEST_PREF_H
#define H2_ALIPAY_TEST_PREF_H

#include "os/h2_pal_pref.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define H2_ALIPAY_TEST_PREF_SPACES 16u
#define H2_ALIPAY_TEST_PREF_ENTRIES 4u
#define H2_ALIPAY_TEST_PREF_HANDLES 4u
#define H2_ALIPAY_TEST_PREF_NAME_BYTES 32u
#define H2_ALIPAY_TEST_PREF_VALUE_BYTES 64u

typedef struct h2_alipay_test_pref_entry {
  char key[H2_ALIPAY_TEST_PREF_NAME_BYTES];
  h2_pal_pref_entry_type_t type;
  size_t size;
  uint8_t value[H2_ALIPAY_TEST_PREF_VALUE_BYTES];
} h2_alipay_test_pref_entry_t;

typedef struct h2_alipay_test_pref_space {
  char name[H2_ALIPAY_TEST_PREF_NAME_BYTES];
  h2_alipay_test_pref_entry_t committed[H2_ALIPAY_TEST_PREF_ENTRIES];
  bool writer;
} h2_alipay_test_pref_space_t;

struct h2_pal_pref_cursor {
  size_t next;
};

typedef struct h2_alipay_test_pref_handle {
  h2_pal_pref_namespace_t api;
  h2_alipay_test_pref_space_t* space;
  bool writable;
  h2_alipay_test_pref_entry_t staged[H2_ALIPAY_TEST_PREF_ENTRIES];
  h2_pal_pref_cursor_t cursor;
} h2_alipay_test_pref_handle_t;

static h2_alipay_test_pref_space_t
    h2_alipay_test_pref_spaces[H2_ALIPAY_TEST_PREF_SPACES];
static h2_alipay_test_pref_handle_t
    h2_alipay_test_pref_handles[H2_ALIPAY_TEST_PREF_HANDLES];

static h2_alipay_test_pref_entry_t* h2_alipay_test_pref_values(
    h2_alipay_test_pref_handle_t* handle) {
  return handle->writable ? handle->staged : handle->space->committed;
}

static h2_alipay_test_pref_entry_t* h2_alipay_test_pref_find(
    h2_alipay_test_pref_entry_t* entries, const char* key) {
  assert(key != NULL && key[0] != '\0' &&
         strlen(key) < H2_ALIPAY_TEST_PREF_NAME_BYTES);
  for (size_t index = 0u; index < H2_ALIPAY_TEST_PREF_ENTRIES; ++index)
    if (strcmp(entries[index].key, key) == 0) return &entries[index];
  return NULL;
}

static int h2_alipay_test_pref_get(h2_pal_pref_namespace_t* ns,
                                   const h2_pal_mem_api_t* mem, const char* key,
                                   void** out, size_t* size,
                                   h2_pal_pref_entry_type_t type) {
  *out = NULL;
  *size = 0u;
  const h2_alipay_test_pref_entry_t* entry =
      h2_alipay_test_pref_find(h2_alipay_test_pref_values(ns->user), key);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (entry->type != type) return H2_PAL_ERR_FORMAT;
  void* copy = h2_pal_mem_alloc(mem, entry->size != 0u ? entry->size : 1u);
  if (copy == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(copy, entry->value, entry->size);
  *out = copy;
  *size = entry->size;
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_get_blob(h2_pal_pref_namespace_t* ns,
                                        const h2_pal_mem_api_t* mem,
                                        const char* key, void** out,
                                        size_t* size) {
  return h2_alipay_test_pref_get(ns, mem, key, out, size,
                                 H2_PAL_PREF_ENTRY_BLOB);
}

static int h2_alipay_test_pref_get_string(h2_pal_pref_namespace_t* ns,
                                          const h2_pal_mem_api_t* mem,
                                          const char* key, char** out) {
  void* data = NULL;
  size_t size = 0u;
  int rc = h2_alipay_test_pref_get(ns, mem, key, &data, &size,
                                   H2_PAL_PREF_ENTRY_STRING);
  *out = data;
  return rc;
}

static int h2_alipay_test_pref_set(h2_pal_pref_namespace_t* ns, const char* key,
                                   h2_pal_pref_entry_type_t type,
                                   const void* data, size_t size) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (!handle->writable) return H2_PAL_ERR_INVALID_STATE;
  assert(size <= H2_ALIPAY_TEST_PREF_VALUE_BYTES);
  h2_alipay_test_pref_entry_t* entry =
      h2_alipay_test_pref_find(handle->staged, key);
  for (size_t index = 0u; entry == NULL && index < H2_ALIPAY_TEST_PREF_ENTRIES;
       ++index)
    if (handle->staged[index].key[0] == '\0') entry = &handle->staged[index];
  assert(entry != NULL);
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->key, key, strlen(key) + 1u);
  entry->type = type;
  entry->size = size;
  if (size != 0u) memcpy(entry->value, data, size);
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_set_blob(h2_pal_pref_namespace_t* ns,
                                        const char* key, const void* data,
                                        size_t size) {
  return h2_alipay_test_pref_set(ns, key, H2_PAL_PREF_ENTRY_BLOB, data, size);
}

static int h2_alipay_test_pref_set_string(h2_pal_pref_namespace_t* ns,
                                          const char* key, const char* value) {
  return h2_alipay_test_pref_set(ns, key, H2_PAL_PREF_ENTRY_STRING, value,
                                 strlen(value) + 1u);
}

static int h2_alipay_test_pref_remove(h2_pal_pref_namespace_t* ns,
                                      const char* key) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (!handle->writable) return H2_PAL_ERR_INVALID_STATE;
  h2_alipay_test_pref_entry_t* entry =
      h2_alipay_test_pref_find(handle->staged, key);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  memset(entry, 0, sizeof(*entry));
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_clear(h2_pal_pref_namespace_t* ns) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (!handle->writable) return H2_PAL_ERR_INVALID_STATE;
  memset(handle->staged, 0, sizeof(handle->staged));
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_commit(h2_pal_pref_namespace_t* ns) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (!handle->writable) return H2_PAL_ERR_INVALID_STATE;
  memcpy(handle->space->committed, handle->staged, sizeof(handle->staged));
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_iterate(h2_pal_pref_namespace_t* ns,
                                       h2_pal_pref_cursor_t** cursor,
                                       h2_pal_pref_entry_t* out) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (*cursor == NULL) {
    handle->cursor.next = 0u;
    *cursor = &handle->cursor;
  }
  h2_alipay_test_pref_entry_t* entries = handle->space->committed;
  while (handle->cursor.next < H2_ALIPAY_TEST_PREF_ENTRIES) {
    const h2_alipay_test_pref_entry_t* entry = &entries[handle->cursor.next++];
    if (entry->key[0] == '\0') continue;
    *out = (h2_pal_pref_entry_t){entry->key, entry->type, entry->size};
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static int h2_alipay_test_pref_iterate_close(h2_pal_pref_namespace_t* ns,
                                             h2_pal_pref_cursor_t** cursor) {
  (void)ns;
  *cursor = NULL;
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_close(h2_pal_pref_namespace_t* ns) {
  h2_alipay_test_pref_handle_t* handle = ns->user;
  if (handle->writable) handle->space->writer = false;
  memset(handle, 0, sizeof(*handle));
  return H2_PAL_OK;
}

static int h2_alipay_test_pref_open(void* user, const char* name,
                                    h2_pal_pref_open_mode_t mode,
                                    h2_pal_pref_namespace_t** out) {
  (void)user;
  assert(out != NULL && name != NULL && name[0] != '\0' &&
         strlen(name) < H2_ALIPAY_TEST_PREF_NAME_BYTES);
  *out = NULL;
  h2_alipay_test_pref_space_t* space = NULL;
  h2_alipay_test_pref_space_t* empty = NULL;
  for (size_t index = 0u; index < H2_ALIPAY_TEST_PREF_SPACES; ++index) {
    h2_alipay_test_pref_space_t* candidate = &h2_alipay_test_pref_spaces[index];
    if (strcmp(candidate->name, name) == 0)
      space = candidate;
    else if (candidate->name[0] == '\0' && empty == NULL)
      empty = candidate;
  }
  bool writable = mode == H2_PAL_PREF_OPEN_READ_WRITE;
  if (space == NULL && !writable) return H2_PAL_ERR_NOT_FOUND;
  if (space != NULL && writable && space->writer) return H2_PAL_ERR_BUSY;
  if (space == NULL) {
    assert(empty != NULL);
    space = empty;
    memcpy(space->name, name, strlen(name) + 1u);
  }
  h2_alipay_test_pref_handle_t* handle = NULL;
  for (size_t index = 0u; handle == NULL && index < H2_ALIPAY_TEST_PREF_HANDLES;
       ++index)
    if (h2_alipay_test_pref_handles[index].space == NULL)
      handle = &h2_alipay_test_pref_handles[index];
  assert(handle != NULL);
  handle->space = space;
  handle->writable = writable;
  if (writable) {
    space->writer = true;
    memcpy(handle->staged, space->committed, sizeof(handle->staged));
  }
  handle->api = (h2_pal_pref_namespace_t){
      .user = handle,
      .close = h2_alipay_test_pref_close,
      .get_blob = h2_alipay_test_pref_get_blob,
      .set_blob = h2_alipay_test_pref_set_blob,
      .get_string = h2_alipay_test_pref_get_string,
      .set_string = h2_alipay_test_pref_set_string,
      .remove = h2_alipay_test_pref_remove,
      .clear = h2_alipay_test_pref_clear,
      .commit = h2_alipay_test_pref_commit,
      .iterate = h2_alipay_test_pref_iterate,
      .iterate_close = h2_alipay_test_pref_iterate_close,
  };
  *out = &handle->api;
  return H2_PAL_OK;
}

static inline const h2_pal_pref_api_t* h2_alipay_test_pref_reset(void) {
  static const h2_pal_pref_vtable_t vtable = {.open = h2_alipay_test_pref_open};
  static const h2_pal_pref_api_t api = {.vtable = &vtable};
  memset(h2_alipay_test_pref_spaces, 0, sizeof(h2_alipay_test_pref_spaces));
  memset(h2_alipay_test_pref_handles, 0, sizeof(h2_alipay_test_pref_handles));
  return &api;
}

static inline bool h2_alipay_test_pref_idle(void) {
  for (size_t index = 0u; index < H2_ALIPAY_TEST_PREF_HANDLES; ++index)
    if (h2_alipay_test_pref_handles[index].space != NULL) return false;
  return true;
}

#endif
