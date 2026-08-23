#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_PREF_NAMESPACE_MAX 64u
#define H2_WEB_PREF_KEY_MAX 96u
#define H2_WEB_PREF_VALUE_MAX (16u * 1024u)

typedef struct h2_web_pref_namespace {
  h2_pal_pref_namespace_t base;
  h2_pal_pref_open_mode_t mode;
  char name_space[H2_WEB_PREF_NAMESPACE_MAX + 1u];
} h2_web_pref_namespace_t;

struct h2_pal_pref_cursor {
  size_t index;
  char key[H2_WEB_PREF_KEY_MAX + 1u];
};

EM_JS(int, h2_web_pref_storage_available_js, (), {
  try {
    const storage = globalThis.localStorage;
    if (!storage) return 0;
    const key = `h2.pref.probe.${Date.now()}.${Math.random()}`;
    storage.setItem(key, '1');
    storage.removeItem(key);
    return 1;
  } catch (_) {
    return 0;
  }
});

EM_JS(int, h2_web_pref_write_js,
      (const char *name_space, const char *key, int type,
       const void *data, size_t data_len), {
        try {
          const storage = globalThis.localStorage;
          if (!storage) return -2;
          const storageKey = `h2.pref.v1.${encodeURIComponent(
              UTF8ToString(name_space))}.${encodeURIComponent(
              UTF8ToString(key))}`;
          const bytes = HEAPU8.subarray(data, data + data_len);
          let hex = "";
          for (const byte of bytes) hex += byte.toString(16).padStart(2, '0');
          storage.setItem(storageKey, `${type}:${hex}`);
          return 0;
        } catch (_) {
          return -4;
        }
      });

EM_JS(int, h2_web_pref_read_js,
      (const char *name_space, const char *key, int expected_type,
       void *out, size_t out_size, size_t *out_len), {
        HEAPU32[out_len >> 2] = 0;
        try {
          const storage = globalThis.localStorage;
          if (!storage) return -2;
          const storageKey = `h2.pref.v1.${encodeURIComponent(
              UTF8ToString(name_space))}.${encodeURIComponent(
              UTF8ToString(key))}`;
          const record = storage.getItem(storageKey);
          if (record === null) return -8;
          const separator = record.indexOf(':');
          if (separator <= 0 || Number(record.slice(0, separator)) !== expected_type) {
            return -15;
          }
          const hex = record.slice(separator + 1);
          if ((hex.length & 1) !== 0) return -15;
          const length = hex.length / 2;
          HEAPU32[out_len >> 2] = length;
          if (length > out_size) return -13;
          for (let index = 0; index < length; ++index) {
            const value = Number.parseInt(hex.slice(index * 2, index * 2 + 2), 16);
            if (!Number.isFinite(value)) return -15;
            HEAPU8[out + index] = value;
          }
          return 0;
        } catch (_) {
          return -4;
        }
      });

EM_JS(int, h2_web_pref_remove_js,
      (const char *name_space, const char *key), {
        try {
          const storage = globalThis.localStorage;
          if (!storage) return -2;
          const storageKey = `h2.pref.v1.${encodeURIComponent(
              UTF8ToString(name_space))}.${encodeURIComponent(
              UTF8ToString(key))}`;
          if (storage.getItem(storageKey) === null) return -8;
          storage.removeItem(storageKey);
          return 0;
        } catch (_) {
          return -4;
        }
      });

EM_JS(int, h2_web_pref_clear_js, (const char *name_space), {
  try {
    const storage = globalThis.localStorage;
    if (!storage) return -2;
    const prefix = `h2.pref.v1.${encodeURIComponent(
        UTF8ToString(name_space))}.`;
    const keys = [];
    for (let index = 0; index < storage.length; ++index) {
      const key = storage.key(index);
      if (key && key.startsWith(prefix)) keys.push(key);
    }
    for (const key of keys) storage.removeItem(key);
    return 0;
  } catch (_) {
    return -4;
  }
});

EM_JS(int, h2_web_pref_iterate_js,
      (const char *name_space, size_t index, char *out_key,
       size_t out_key_size, int *out_type, size_t *out_value_size), {
        try {
          const storage = globalThis.localStorage;
          if (!storage) return -2;
          const prefix = `h2.pref.v1.${encodeURIComponent(
              UTF8ToString(name_space))}.`;
          const keys = [];
          for (let cursor = 0; cursor < storage.length; ++cursor) {
            const key = storage.key(cursor);
            if (key && key.startsWith(prefix)) keys.push(key);
          }
          keys.sort();
          if (index >= keys.length) return -8;
          const storageKey = keys[index];
          const record = storage.getItem(storageKey);
          const separator = record ? record.indexOf(':') : -1;
          if (separator <= 0) return -15;
          const type = Number(record.slice(0, separator));
          const hex = record.slice(separator + 1);
          if (!Number.isInteger(type) || type < 1 || type > 5 ||
              (hex.length & 1) !== 0) return -15;
          const key = decodeURIComponent(storageKey.slice(prefix.length));
          if (lengthBytesUTF8(key) + 1 > out_key_size) return -13;
          stringToUTF8(key, out_key, out_key_size);
          HEAP32[out_type >> 2] = type;
          HEAPU32[out_value_size >> 2] = hex.length / 2;
          return 0;
        } catch (_) {
          return -4;
        }
      });

static size_t bounded_strlen(const char *text, size_t max_len) {
  size_t length = 0u;
  if (text == NULL) return 0u;
  while (length <= max_len && text[length] != '\0') ++length;
  return length;
}

static int valid_text(const char *text, size_t max_len) {
  const size_t length = bounded_strlen(text, max_len);
  return text != NULL && length > 0u && length <= max_len;
}

static h2_web_pref_namespace_t *pref_namespace(
    h2_pal_pref_namespace_t *name_space) {
  return name_space == NULL ? NULL : name_space->user;
}

static int pref_close(h2_pal_pref_namespace_t *name_space) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  if (store == NULL) return H2_PAL_ERR_INVALID_ARG;
  free(store);
  return H2_PAL_OK;
}

static int pref_read(h2_web_pref_namespace_t *store, const char *key,
                     h2_pal_pref_entry_type_t type, void *out,
                     size_t out_size, size_t *out_len) {
  if (store == NULL || !valid_text(key, H2_WEB_PREF_KEY_MAX) ||
      out_len == NULL || (out == NULL && out_size != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_web_pref_read_js(store->name_space, key, type, out, out_size,
                             out_len);
}

static int pref_write(h2_web_pref_namespace_t *store, const char *key,
                      h2_pal_pref_entry_type_t type, const void *data,
                      size_t data_len) {
  if (store == NULL || store->mode != H2_PAL_PREF_OPEN_READ_WRITE ||
      !valid_text(key, H2_WEB_PREF_KEY_MAX) ||
      (data == NULL && data_len != 0u) || data_len > H2_WEB_PREF_VALUE_MAX) {
    return store != NULL && store->mode != H2_PAL_PREF_OPEN_READ_WRITE
               ? H2_PAL_ERR_INVALID_STATE
               : H2_PAL_ERR_INVALID_ARG;
  }
  return h2_web_pref_write_js(store->name_space, key, type, data, data_len);
}

static int pref_get_blob(h2_pal_pref_namespace_t *name_space,
                         const h2_pal_mem_api_t *allocator, const char *key,
                         void **out_data, size_t *out_len) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  size_t length = 0u;
  int rc;
  if (store == NULL || allocator == NULL || out_data == NULL ||
      out_len == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_data = NULL;
  *out_len = 0u;
  rc = pref_read(store, key, H2_PAL_PREF_ENTRY_BLOB, NULL, 0u, &length);
  if (rc != H2_PAL_ERR_NO_SPACE && !(rc == H2_PAL_OK && length == 0u)) {
    return rc;
  }
  void *data = h2_pal_mem_alloc(allocator, length == 0u ? 1u : length);
  if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
  rc = pref_read(store, key, H2_PAL_PREF_ENTRY_BLOB, data, length, &length);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, data);
    return rc;
  }
  *out_data = data;
  *out_len = length;
  return H2_PAL_OK;
}

static int pref_set_blob(h2_pal_pref_namespace_t *name_space,
                         const char *key, const void *data, size_t data_len) {
  return pref_write(pref_namespace(name_space), key, H2_PAL_PREF_ENTRY_BLOB,
                    data, data_len);
}

static int pref_get_string(h2_pal_pref_namespace_t *name_space,
                           const h2_pal_mem_api_t *allocator, const char *key,
                           char **out_value) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  size_t length = 0u;
  int rc;
  if (store == NULL || allocator == NULL || out_value == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_value = NULL;
  rc = pref_read(store, key, H2_PAL_PREF_ENTRY_STRING, NULL, 0u, &length);
  if (rc != H2_PAL_ERR_NO_SPACE && !(rc == H2_PAL_OK && length == 0u)) {
    return rc;
  }
  char *value = h2_pal_mem_alloc(allocator, length + 1u);
  if (value == NULL) return H2_PAL_ERR_NO_MEMORY;
  rc = pref_read(store, key, H2_PAL_PREF_ENTRY_STRING, value, length, &length);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, value);
    return rc;
  }
  value[length] = '\0';
  *out_value = value;
  return H2_PAL_OK;
}

static int pref_set_string(h2_pal_pref_namespace_t *name_space,
                           const char *key, const char *value) {
  if (value == NULL) return H2_PAL_ERR_INVALID_ARG;
  const size_t length = bounded_strlen(value, H2_WEB_PREF_VALUE_MAX);
  if (length > H2_WEB_PREF_VALUE_MAX) return H2_PAL_ERR_INVALID_ARG;
  return pref_write(pref_namespace(name_space), key,
                    H2_PAL_PREF_ENTRY_STRING, value, length);
}

static int pref_get_u32(h2_pal_pref_namespace_t *name_space, const char *key,
                        uint32_t *out_value) {
  size_t length = 0u;
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int rc = pref_read(pref_namespace(name_space), key,
                           H2_PAL_PREF_ENTRY_U32, out_value,
                           sizeof(*out_value), &length);
  return rc == H2_PAL_OK && length != sizeof(*out_value)
             ? H2_PAL_ERR_FORMAT
             : rc;
}

static int pref_set_u32(h2_pal_pref_namespace_t *name_space, const char *key,
                        uint32_t value) {
  return pref_write(pref_namespace(name_space), key, H2_PAL_PREF_ENTRY_U32,
                    &value, sizeof(value));
}

static int pref_get_i32(h2_pal_pref_namespace_t *name_space, const char *key,
                        int32_t *out_value) {
  size_t length = 0u;
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int rc = pref_read(pref_namespace(name_space), key,
                           H2_PAL_PREF_ENTRY_I32, out_value,
                           sizeof(*out_value), &length);
  return rc == H2_PAL_OK && length != sizeof(*out_value)
             ? H2_PAL_ERR_FORMAT
             : rc;
}

static int pref_set_i32(h2_pal_pref_namespace_t *name_space, const char *key,
                        int32_t value) {
  return pref_write(pref_namespace(name_space), key, H2_PAL_PREF_ENTRY_I32,
                    &value, sizeof(value));
}

static int pref_get_bool(h2_pal_pref_namespace_t *name_space, const char *key,
                         int *out_value) {
  int32_t stored = 0;
  size_t length = 0u;
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int rc = pref_read(pref_namespace(name_space), key,
                           H2_PAL_PREF_ENTRY_BOOL, &stored, sizeof(stored),
                           &length);
  if (rc != H2_PAL_OK) return rc;
  if (length != sizeof(stored) || (stored != 0 && stored != 1)) {
    return H2_PAL_ERR_FORMAT;
  }
  *out_value = stored;
  return H2_PAL_OK;
}

static int pref_set_bool(h2_pal_pref_namespace_t *name_space, const char *key,
                         int value) {
  const int32_t stored = value != 0 ? 1 : 0;
  return pref_write(pref_namespace(name_space), key, H2_PAL_PREF_ENTRY_BOOL,
                    &stored, sizeof(stored));
}

static int pref_remove(h2_pal_pref_namespace_t *name_space, const char *key) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  if (store == NULL || !valid_text(key, H2_WEB_PREF_KEY_MAX)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (store->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_web_pref_remove_js(store->name_space, key);
}

static int pref_clear(h2_pal_pref_namespace_t *name_space) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  if (store == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (store->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_web_pref_clear_js(store->name_space);
}

static int pref_commit(h2_pal_pref_namespace_t *name_space) {
  return pref_namespace(name_space) == NULL ? H2_PAL_ERR_INVALID_ARG
                                             : H2_PAL_OK;
}

static int pref_iterate(h2_pal_pref_namespace_t *name_space,
                        h2_pal_pref_cursor_t **cursor,
                        h2_pal_pref_entry_t *out_entry) {
  h2_web_pref_namespace_t *store = pref_namespace(name_space);
  if (store == NULL || cursor == NULL || out_entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (*cursor == NULL) {
    *cursor = calloc(1u, sizeof(**cursor));
    if (*cursor == NULL) return H2_PAL_ERR_NO_MEMORY;
  }
  int type = H2_PAL_PREF_ENTRY_UNKNOWN;
  size_t value_size = 0u;
  const int rc = h2_web_pref_iterate_js(
      store->name_space, (*cursor)->index, (*cursor)->key,
      sizeof((*cursor)->key), &type, &value_size);
  if (rc != H2_PAL_OK) {
    if (rc == H2_PAL_ERR_NOT_FOUND) {
      free(*cursor);
      *cursor = NULL;
    }
    return rc;
  }
  out_entry->key = (*cursor)->key;
  out_entry->type = (h2_pal_pref_entry_type_t)type;
  out_entry->value_size = value_size;
  ++(*cursor)->index;
  return H2_PAL_OK;
}

static int pref_iterate_close(h2_pal_pref_namespace_t *name_space,
                              h2_pal_pref_cursor_t **cursor) {
  if (pref_namespace(name_space) == NULL || cursor == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  free(*cursor);
  *cursor = NULL;
  return H2_PAL_OK;
}

static int pref_open(void *user, const char *name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t **out_namespace) {
  h2_web_platform_t *platform = user;
  if (out_namespace != NULL) *out_namespace = NULL;
  if (platform == NULL || out_namespace == NULL ||
      !valid_text(name_space, H2_WEB_PREF_NAMESPACE_MAX) ||
      (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
       mode != H2_PAL_PREF_OPEN_READ_WRITE)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_web_pref_storage_available_js()) return H2_PAL_ERR_UNAVAILABLE;
  h2_web_pref_namespace_t *store = calloc(1u, sizeof(*store));
  if (store == NULL) return H2_PAL_ERR_NO_MEMORY;
  store->mode = mode;
  memcpy(store->name_space, name_space, strlen(name_space) + 1u);
  store->base.user = store;
  store->base.close = pref_close;
  store->base.get_blob = pref_get_blob;
  store->base.set_blob = pref_set_blob;
  store->base.get_string = pref_get_string;
  store->base.set_string = pref_set_string;
  store->base.get_u32 = pref_get_u32;
  store->base.set_u32 = pref_set_u32;
  store->base.get_i32 = pref_get_i32;
  store->base.set_i32 = pref_set_i32;
  store->base.get_bool = pref_get_bool;
  store->base.set_bool = pref_set_bool;
  store->base.remove = pref_remove;
  store->base.clear = pref_clear;
  store->base.commit = pref_commit;
  store->base.iterate = pref_iterate;
  store->base.iterate_close = pref_iterate_close;
  *out_namespace = &store->base;
  return H2_PAL_OK;
}

static const h2_pal_pref_vtable_t pref_vtable = {
    .open = pref_open,
};

void h2_web_platform_pref_init(h2_web_platform_t *platform) {
  platform->pref_api.user = platform;
  platform->pref_api.vtable = &pref_vtable;
}
