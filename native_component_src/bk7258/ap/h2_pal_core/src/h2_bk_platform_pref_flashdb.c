#include "h2_bk_platform_core.h"

#include "easyflash.h"
#include "flashdb.h"
#include "os/os.h"
#include "os/mem.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Storage keys are "<namespace>.<key>"; FlashDB accepts names up to
 * FDB_KV_NAME_MAX, e.g. "h2loader.mfg_acceptance_revision" (32 chars). */
#define H2_BK_PREF_KEY_MAX FDB_KV_NAME_MAX

typedef struct h2_bk_pref_namespace {
    h2_pal_pref_namespace_t base;
    char name_space[16];
    h2_pal_pref_open_mode_t mode;
} h2_bk_pref_namespace_t;

static struct fdb_kvdb s_pref_database;
static beken_mutex_t s_pref_database_mutex;
static beken_mutex_t s_pref_pool_mutex;
static beken_mutex_t s_pref_operation_mutex;
static int s_pref_database_ready;
static int s_pref_pool_mutex_ready;

static int bk_pref_map_flashdb_error(fdb_err_t rc) {
    switch (rc) {
    case FDB_NO_ERR:
        return H2_PAL_OK;
    case FDB_KV_NAME_ERR:
    case FDB_KV_NAME_EXIST:
        return H2_PAL_ERR_INVALID_ARG;
    case FDB_SAVED_FULL:
        return H2_PAL_ERR_NO_SPACE;
    case FDB_INIT_FAILED:
    case FDB_PART_NOT_FOUND:
        return H2_PAL_ERR_UNAVAILABLE;
    case FDB_ERASE_ERR:
    case FDB_READ_ERR:
    case FDB_WRITE_ERR:
    default:
        return H2_PAL_ERR_IO;
    }
}

static h2_bk_pref_namespace_t *bk_pref_to_namespace(h2_pal_pref_namespace_t *ns) {
    return (h2_bk_pref_namespace_t *)ns;
}

static int bk_pref_lock_pool(void) {
    if (!s_pref_pool_mutex_ready) {
        if (rtos_init_mutex(&s_pref_pool_mutex) != kNoErr) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        s_pref_pool_mutex_ready = 1;
    }
    return rtos_lock_mutex(&s_pref_pool_mutex) == kNoErr ?
        H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
}

static void bk_pref_unlock_pool(void) {
    if (s_pref_pool_mutex_ready) {
        (void)rtos_unlock_mutex(&s_pref_pool_mutex);
    }
}

static void bk_pref_lock_database(fdb_db_t database) {
    beken_mutex_t *mutex = database != NULL ?
        (beken_mutex_t *)database->user_data : NULL;

    if (mutex != NULL) {
        (void)rtos_lock_mutex(mutex);
    }
}

static void bk_pref_unlock_database(fdb_db_t database) {
    beken_mutex_t *mutex = database != NULL ?
        (beken_mutex_t *)database->user_data : NULL;

    if (mutex != NULL) {
        (void)rtos_unlock_mutex(mutex);
    }
}

static int bk_pref_init_database(void) {
    int rc;

    rc = bk_pref_lock_pool();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (s_pref_database_ready) {
        bk_pref_unlock_pool();
        return H2_PAL_OK;
    }
    if (rtos_init_mutex(&s_pref_database_mutex) != kNoErr) {
        bk_pref_unlock_pool();
        return H2_PAL_ERR_UNAVAILABLE;
    }
    if (rtos_init_mutex(&s_pref_operation_mutex) != kNoErr) {
        (void)rtos_deinit_mutex(&s_pref_database_mutex);
        bk_pref_unlock_pool();
        return H2_PAL_ERR_UNAVAILABLE;
    }
    fdb_kvdb_control(
        &s_pref_database,
        FDB_KVDB_CTRL_SET_LOCK,
        (void *)bk_pref_lock_database);
    fdb_kvdb_control(
        &s_pref_database,
        FDB_KVDB_CTRL_SET_UNLOCK,
        (void *)bk_pref_unlock_database);
    rc = bk_pref_map_flashdb_error(fdb_kvdb_init(
        &s_pref_database,
        "h2_pref",
        H2_BK_PREF_FLASHDB_PATH,
        NULL,
        &s_pref_database_mutex));
    if (rc == H2_PAL_OK) {
        s_pref_database_ready = 1;
    } else {
        (void)rtos_deinit_mutex(&s_pref_operation_mutex);
        (void)rtos_deinit_mutex(&s_pref_database_mutex);
    }
    bk_pref_unlock_pool();
    return rc;
}

static int bk_pref_make_key(
    const h2_bk_pref_namespace_t *ns,
    const char *key,
    char out[H2_BK_PREF_KEY_MAX]) {
    int written;

    if (ns == NULL || key == NULL || key[0] == '\0' || out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    written = snprintf(out, H2_BK_PREF_KEY_MAX, "%s.%s", ns->name_space, key);
    if (written < 0 || (size_t)written >= H2_BK_PREF_KEY_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static int bk_pref_require_writable(const h2_bk_pref_namespace_t *ns) {
    if (ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return ns->mode == H2_PAL_PREF_OPEN_READ_WRITE ?
        H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static int bk_pref_delete_easyflash_value(const char *key) {
    size_t saved_len = 0u;

    if (key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (easyflash_init() != EF_NO_ERR) {
        return H2_PAL_ERR_IO;
    }
    (void)ef_get_env_blob(key, NULL, 0u, &saved_len);
    if (saved_len == 0u) {
        return H2_PAL_OK;
    }
    return ef_del_env(key) == EF_NO_ERR ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int bk_pref_find_value_size(const char *key, size_t *out_len) {
    struct fdb_kv kv;
    size_t easyflash_len = 0u;

    if (key == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_len = 0u;
    if (easyflash_init() == EF_NO_ERR) {
        (void)ef_get_env_blob(key, NULL, 0u, &easyflash_len);
    }
    if (easyflash_len != 0u) {
        *out_len = easyflash_len;
        return H2_PAL_OK;
    }
    memset(&kv, 0, sizeof(kv));
    if (fdb_kv_get_obj(&s_pref_database, key, &kv) != NULL) {
        *out_len = kv.value_len;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int bk_pref_read_value(
    const h2_bk_pref_namespace_t *ns,
    const char *key,
    void *data,
    size_t capacity,
    size_t *out_len) {
    struct fdb_blob blob;
    struct fdb_kv kv;
    size_t saved_len = 0u;

    if (ns == NULL || key == NULL || data == NULL || capacity == 0u ||
        out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_len = 0u;
    if (easyflash_init() == EF_NO_ERR) {
        (void)ef_get_env_blob(key, NULL, 0u, &saved_len);
    }
    if (saved_len != 0u) {
        if (saved_len > capacity) {
            return H2_PAL_ERR_NO_SPACE;
        }
        if (ef_get_env_blob(key, data, capacity, NULL) != saved_len) {
            return H2_PAL_ERR_IO;
        }
        if (ns->mode == H2_PAL_PREF_OPEN_READ_WRITE) {
            int migration_rc = bk_pref_map_flashdb_error(fdb_kv_set_blob(
                &s_pref_database,
                key,
                fdb_blob_make(&blob, data, saved_len)));
            if (migration_rc != H2_PAL_OK) {
                return migration_rc;
            }
            migration_rc = bk_pref_delete_easyflash_value(key);
            if (migration_rc != H2_PAL_OK) {
                return migration_rc;
            }
        }
        *out_len = saved_len;
        return H2_PAL_OK;
    }

    memset(&kv, 0, sizeof(kv));
    if (fdb_kv_get_obj(&s_pref_database, key, &kv) == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (kv.value_len > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (fdb_kv_get_blob(
            &s_pref_database,
            key,
            fdb_blob_make(&blob, data, capacity)) != kv.value_len) {
        return H2_PAL_ERR_IO;
    }
    *out_len = kv.value_len;
    return H2_PAL_OK;
}

static int bk_pref_close(h2_pal_pref_namespace_t *ns) {
    h2_bk_pref_namespace_t *pref_ns = bk_pref_to_namespace(ns);

    if (pref_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    os_free(pref_ns);
    return H2_PAL_OK;
}

static int bk_pref_get_blob(
    h2_pal_pref_namespace_t *base,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
    char storage_key[H2_BK_PREF_KEY_MAX];
    size_t saved_len = 0u;
    void *data;
    int rc;

    if (allocator == NULL || out_data == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0u;
    rc = bk_pref_make_key(ns, key, storage_key);
    if (rc == H2_PAL_OK) {
        rc = bk_pref_find_value_size(storage_key, &saved_len);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    data = h2_pal_mem_alloc(allocator, saved_len);
    if (data == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    rc = bk_pref_read_value(ns, storage_key, data, saved_len, out_len);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(allocator, data);
        return rc;
    }
    *out_data = data;
    return H2_PAL_OK;
}

static int bk_pref_set_blob(
    h2_pal_pref_namespace_t *base,
    const char *key,
    const void *data,
    size_t data_len) {
    h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
    struct fdb_blob blob;
    char storage_key[H2_BK_PREF_KEY_MAX];
    int rc;

    rc = bk_pref_require_writable(ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (data == NULL || data_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bk_pref_make_key(ns, key, storage_key);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bk_pref_map_flashdb_error(fdb_kv_set_blob(
        &s_pref_database,
        storage_key,
        fdb_blob_make(&blob, data, data_len)));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return bk_pref_delete_easyflash_value(storage_key);
}

static int bk_pref_get_string(
    h2_pal_pref_namespace_t *base,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value) {
    void *data = NULL;
    size_t len = 0u;
    char *value;
    int rc;

    if (allocator == NULL || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    rc = bk_pref_get_blob(base, allocator, key, &data, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    value = (char *)h2_pal_mem_alloc(allocator, len + 1u);
    if (value == NULL) {
        h2_pal_mem_free(allocator, data);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(value, data, len);
    value[len] = '\0';
    h2_pal_mem_free(allocator, data);
    *out_value = value;
    return H2_PAL_OK;
}

static int bk_pref_get_fixed(
    h2_pal_pref_namespace_t *base,
    const char *key,
    void *data,
    size_t expected_len) {
    h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
    char storage_key[H2_BK_PREF_KEY_MAX];
    size_t saved_len = 0u;
    int rc;

    if (data == NULL || expected_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bk_pref_make_key(ns, key, storage_key);
    if (rc == H2_PAL_OK) {
        rc = bk_pref_read_value(
            ns,
            storage_key,
            data,
            expected_len,
            &saved_len);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return saved_len == expected_len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int bk_pref_get_u32(h2_pal_pref_namespace_t *base, const char *key, uint32_t *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0u;
    return bk_pref_get_fixed(base, key, out_value, sizeof(*out_value));
}

static int bk_pref_get_i32(h2_pal_pref_namespace_t *base, const char *key, int32_t *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0;
    return bk_pref_get_fixed(base, key, out_value, sizeof(*out_value));
}

static int bk_pref_get_bool(h2_pal_pref_namespace_t *base, const char *key, int *out_value) {
    uint8_t value = 0u;
    int rc;

    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0;
    rc = bk_pref_get_fixed(base, key, &value, sizeof(value));
    if (rc == H2_PAL_OK) {
        *out_value = value != 0u;
    }
    return rc;
}

static int bk_pref_remove(h2_pal_pref_namespace_t *base, const char *key) {
    h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
    struct fdb_kv kv;
    char storage_key[H2_BK_PREF_KEY_MAX];
    size_t easyflash_len = 0u;
    int found = 0;
    int rc;

    rc = bk_pref_require_writable(ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bk_pref_make_key(ns, key, storage_key);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(&kv, 0, sizeof(kv));
    if (fdb_kv_get_obj(&s_pref_database, storage_key, &kv) != NULL) {
        rc = bk_pref_map_flashdb_error(
            fdb_kv_del(&s_pref_database, storage_key));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        found = 1;
    }
    if (easyflash_init() != EF_NO_ERR) {
        return H2_PAL_ERR_IO;
    }
    (void)ef_get_env_blob(storage_key, NULL, 0u, &easyflash_len);
    if (easyflash_len != 0u) {
        if (ef_del_env(storage_key) != EF_NO_ERR) {
            return H2_PAL_ERR_IO;
        }
        found = 1;
    }
    return found ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static int bk_pref_commit(h2_pal_pref_namespace_t *base) {
    (void)base;
    return H2_PAL_OK;
}

/* Keep legacy value bytes readable by the installed Loader. Optional type
 * metadata lives in a separate, reserved FlashDB key. Its key-name and payload
 * digest reject collisions and stale metadata after writes by older firmware.
 */
static uint64_t type_hash(const void *bytes, size_t size) {
  const uint8_t *p = bytes;
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i)
    h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}
static void metadata_key(const char *storage_key, char out[32]) {
  uint64_t hash = type_hash(storage_key, strlen(storage_key));
  snprintf(out, 32, "$h2t.%08lx%08lx", (unsigned long)(hash >> 32),
           (unsigned long)(hash & UINT32_MAX));
}
static void put_u64(uint8_t *out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    out[i] = (uint8_t)(value >> (8 * i));
}
static uint64_t get_u64(const uint8_t *in) {
  uint64_t out = 0;
  for (unsigned i = 0; i < 8; ++i)
    out |= (uint64_t)in[i] << (8 * i);
  return out;
}
/* One FlashDB metadata write stages both descriptions before the raw value.
 * Readers select the description matching the actual bytes, so a failed value
 * write needs no compensating FlashDB write (which could fail for the same
 * reason). A differing byte also disambiguates equal-length hash collisions. */
#define TYPE_V1_SIZE (24u + H2_BK_PREF_KEY_MAX)
#define TYPE_ENTRY_SIZE 32u
#define TYPE_V2_KEY_OFFSET (8u + 2u * TYPE_ENTRY_SIZE)
#define TYPE_V2_SIZE (TYPE_V2_KEY_OFFSET + H2_BK_PREF_KEY_MAX)

static void describe_value(uint8_t *entry, h2_pal_pref_entry_type_t type,
                           const uint8_t *data, size_t length, size_t probe) {
  entry[0] = (uint8_t)type;
  entry[1] = length ? data[probe] : 0u;
  put_u64(entry + 8, length);
  put_u64(entry + 16, type_hash(data, length));
  put_u64(entry + 24, probe);
}
static int metadata_type(const uint8_t *record, size_t record_size,
                         const char *full, const uint8_t *data, size_t length,
                         h2_pal_pref_entry_type_t *out_type) {
  *out_type = H2_PAL_PREF_ENTRY_UNKNOWN;
  uint64_t hash = type_hash(data, length);
  if (record_size == TYPE_V1_SIZE && !memcmp(record, "H2TYPE1", 7)) {
    if (memcmp(record + 24, full, strlen(full) + 1) ||
        record[7] < H2_PAL_PREF_ENTRY_BLOB || record[7] > H2_PAL_PREF_ENTRY_BOOL)
      return H2_PAL_ERR_FORMAT;
    if (length == get_u64(record + 8) && hash == get_u64(record + 16))
      *out_type = (h2_pal_pref_entry_type_t)record[7];
    return H2_PAL_OK;
  }
  if (record_size != TYPE_V2_SIZE || memcmp(record, "H2TYPE2", 7) ||
      record[7] < 1u || record[7] > 2u ||
      memcmp(record + TYPE_V2_KEY_OFFSET, full, strlen(full) + 1))
    return H2_PAL_ERR_FORMAT;
  int matched = 0;
  for (unsigned i = 0; i < record[7]; ++i) {
    const uint8_t *entry = record + 8u + i * TYPE_ENTRY_SIZE;
    uint64_t saved_length = get_u64(entry + 8);
    uint64_t probe = get_u64(entry + 24);
    if (entry[0] > H2_PAL_PREF_ENTRY_BOOL ||
        (saved_length ? probe >= saved_length : probe != 0u))
      return H2_PAL_ERR_FORMAT;
    if (saved_length == length && get_u64(entry + 16) == hash &&
        (!length || entry[1] == data[(size_t)probe])) {
      if (matched)
        return H2_PAL_ERR_FORMAT;
      *out_type = (h2_pal_pref_entry_type_t)entry[0];
      matched = 1;
    }
  }
  return H2_PAL_OK;
}
static int read_metadata_type(const char *full, const uint8_t *data,
                              size_t length, h2_pal_pref_entry_type_t *type) {
  char key[32];
  uint8_t record[TYPE_V2_SIZE] = {0};
  struct fdb_blob blob;
  struct fdb_kv item = {0};
  *type = H2_PAL_PREF_ENTRY_UNKNOWN;
  metadata_key(full, key);
  if (!fdb_kv_get_obj(&s_pref_database, key, &item))
    return H2_PAL_OK;
  if ((item.value_len != TYPE_V1_SIZE && item.value_len != TYPE_V2_SIZE) ||
      fdb_kv_get_blob(&s_pref_database, key,
                      fdb_blob_make(&blob, record, sizeof(record))) != item.value_len)
    return H2_PAL_ERR_FORMAT;
  return metadata_type(record, item.value_len, full, data, length, type);
}
static int load_value(h2_bk_pref_namespace_t *ns, const char *full,
                       uint8_t **data, size_t *length) {
  *data = NULL;
  int rc = bk_pref_find_value_size(full, length);
  if (rc)
    return rc;
  uint8_t *value = os_malloc(*length ? *length : 1u);
  if (!value)
    return H2_PAL_ERR_NO_MEMORY;
  size_t actual = 0;
  rc = bk_pref_read_value(ns, full, value, *length ? *length : 1u, &actual);
  if (!rc && actual != *length)
    rc = H2_PAL_ERR_IO;
  if (rc) {
    os_free(value);
    return rc;
  }
  *data = value;
  return H2_PAL_OK;
}
static int value_type(h2_bk_pref_namespace_t *ns, const char *key,
                      h2_pal_pref_entry_type_t *out_type) {
  char full[H2_BK_PREF_KEY_MAX];
  uint8_t *value = NULL;
  size_t length = 0;
  *out_type = H2_PAL_PREF_ENTRY_UNKNOWN;
  int rc = bk_pref_make_key(ns, key, full);
  if (!rc) {
    /* Type checks and iterator snapshots must not migrate keys mid-iteration. */
    h2_bk_pref_namespace_t observer = *ns;
    observer.mode = H2_PAL_PREF_OPEN_READ_ONLY;
    rc = load_value(&observer, full, &value, &length);
  }
  if (!rc)
    rc = read_metadata_type(full, value, length, out_type);
  os_free(value);
  return rc;
}
static int check_type(h2_pal_pref_namespace_t *base, const char *key,
                      h2_pal_pref_entry_type_t expected) {
  h2_pal_pref_entry_type_t found;
  int rc = value_type(bk_pref_to_namespace(base), key, &found);
  if (rc)
    return rc;
  return found == H2_PAL_PREF_ENTRY_UNKNOWN || found == expected
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}
static int storage_lock(void) {
  return rtos_lock_mutex(&s_pref_operation_mutex) == kNoErr
             ? H2_PAL_OK
             : H2_PAL_ERR_UNAVAILABLE;
}
static int storage_unlock(int rc) {
  (void)rtos_unlock_mutex(&s_pref_operation_mutex);
  return rc;
}
static int typed_get_blob(h2_pal_pref_namespace_t *base,
                          const h2_pal_mem_api_t *allocator, const char *key,
                          void **out, size_t *length) {
  if (!out || !length)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  *length = 0;
  int rc = storage_lock();
  if (rc)
    return rc;
  rc = check_type(base, key, H2_PAL_PREF_ENTRY_BLOB);
  if (!rc)
    rc = bk_pref_get_blob(base, allocator, key, out, length);
  return storage_unlock(rc);
}
static int typed_set_value(h2_pal_pref_namespace_t *base, const char *key,
                           const void *data, size_t length,
                           h2_pal_pref_entry_type_t type) {
  h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
  char full[H2_BK_PREF_KEY_MAX], meta_key[32];
  int rc = bk_pref_require_writable(ns);
  if (!rc && (!data || !length))
    rc = H2_PAL_ERR_INVALID_ARG;
  if (!rc)
    rc = bk_pref_make_key(ns, key, full);
  if (rc)
    return rc;
  rc = storage_lock();
  if (rc)
    return rc;
  uint8_t *previous = NULL;
  size_t previous_length = 0;
  h2_pal_pref_entry_type_t previous_type = H2_PAL_PREF_ENTRY_UNKNOWN;
  rc = load_value(ns, full, &previous, &previous_length);
  int existed = rc == H2_PAL_OK;
  if (rc == H2_PAL_ERR_NOT_FOUND)
    rc = H2_PAL_OK;
  if (!rc && existed)
    rc = read_metadata_type(full, previous, previous_length, &previous_type);
  if (!rc) {
    size_t probe = 0;
    int same = existed && previous_length == length &&
               !memcmp(previous, data, length);
    if (existed && !same && previous_length == length) {
      while (previous[probe] == ((const uint8_t *)data)[probe])
        ++probe;
    }
    uint8_t record[TYPE_V2_SIZE] = {0};
    memcpy(record, "H2TYPE2", 7);
    record[7] = (uint8_t)(existed && !same ? 2u : 1u);
    describe_value(record + 8, type, data, length, probe);
    if (record[7] == 2u)
      describe_value(record + 8 + TYPE_ENTRY_SIZE, previous_type,
                     previous, previous_length, probe);
    memcpy(record + TYPE_V2_KEY_OFFSET, full, strlen(full) + 1);
    metadata_key(full, meta_key);
    struct fdb_blob blob;
    rc = bk_pref_map_flashdb_error(fdb_kv_set_blob(
        &s_pref_database, meta_key, fdb_blob_make(&blob, record, sizeof(record))));
    /* Identical payloads (including type-only changes) commit with metadata
     * alone. load_value has already completed any legacy migration. */
    if (!rc && !same)
      rc = bk_pref_set_blob(base, key, data, length);
  }
  os_free(previous);
  return storage_unlock(rc);
}
static int typed_set_blob(h2_pal_pref_namespace_t *base, const char *key,
                          const void *data, size_t size) {
  return typed_set_value(base, key, data, size, H2_PAL_PREF_ENTRY_BLOB);
}
static int typed_get_string(h2_pal_pref_namespace_t *base,
                            const h2_pal_mem_api_t *allocator, const char *key,
                            char **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = storage_lock();
  if (rc)
    return rc;
  rc = check_type(base, key, H2_PAL_PREF_ENTRY_STRING);
  if (!rc)
    rc = bk_pref_get_string(base, allocator, key, out);
  return storage_unlock(rc);
}
static int typed_set_string(h2_pal_pref_namespace_t *base, const char *key,
                            const char *value) {
  if (!value)
    return H2_PAL_ERR_INVALID_ARG;
  return typed_set_value(base, key, value, strlen(value), H2_PAL_PREF_ENTRY_STRING);
}
#define TYPED_NUMBER(suffix, ctype, kind)                                      \
  static int typed_get_##suffix(h2_pal_pref_namespace_t *base,                 \
                                const char *key, ctype *out) {                 \
    if (!out)                                                                  \
      return H2_PAL_ERR_INVALID_ARG;                                           \
    *out = 0;                                                                  \
    int rc = storage_lock();                                                   \
    if (rc)                                                                    \
      return rc;                                                               \
    rc = check_type(base, key, kind);                                          \
    if (!rc)                                                                   \
      rc = bk_pref_get_##suffix(base, key, out);                               \
    return storage_unlock(rc);                                                 \
  }                                                                            \
  static int typed_set_##suffix(h2_pal_pref_namespace_t *base,                 \
                                const char *key, ctype value) {                \
    return typed_set_value(base, key, &value, sizeof(value), kind);              \
  }
TYPED_NUMBER(u32, uint32_t, H2_PAL_PREF_ENTRY_U32)
TYPED_NUMBER(i32, int32_t, H2_PAL_PREF_ENTRY_I32)
static int typed_get_bool(h2_pal_pref_namespace_t *base, const char *key,
                          int *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  int rc = storage_lock();
  if (rc)
    return rc;
  rc = check_type(base, key, H2_PAL_PREF_ENTRY_BOOL);
  if (!rc)
    rc = bk_pref_get_bool(base, key, out);
  return storage_unlock(rc);
}
static int typed_set_bool(h2_pal_pref_namespace_t *base, const char *key,
                          int value) {
  uint8_t stored = value ? 1 : 0;
  return typed_set_value(base, key, &stored, sizeof(stored), H2_PAL_PREF_ENTRY_BOOL);
}
static int remove_value(h2_pal_pref_namespace_t *base, const char *key) {
  int value_rc = bk_pref_remove(base, key);
  if (value_rc != H2_PAL_OK && value_rc != H2_PAL_ERR_NOT_FOUND)
    return value_rc;
  int rc;
  char full[H2_BK_PREF_KEY_MAX], meta_key[32];
  rc = bk_pref_make_key(bk_pref_to_namespace(base), key, full);
  if (rc)
    return rc;
  metadata_key(full, meta_key);
  struct fdb_kv item = {0};
  rc = fdb_kv_get_obj(&s_pref_database, meta_key, &item)
           ? bk_pref_map_flashdb_error(fdb_kv_del(&s_pref_database, meta_key))
           : H2_PAL_OK;
  return rc ? rc : value_rc;
}
static int typed_remove(h2_pal_pref_namespace_t *base, const char *key) {
  int rc = storage_lock();
  if (rc)
    return rc;
  return storage_unlock(remove_value(base, key));
}
typedef struct pref_snapshot_entry {
  char key[H2_BK_PREF_KEY_MAX];
  h2_pal_pref_entry_type_t type;
  size_t length;
} pref_snapshot_entry_t;
struct h2_pal_pref_cursor {
  h2_pal_pref_namespace_t *owner;
  size_t index, count;
  pref_snapshot_entry_t *entries;
};
/* Failed creation can leave a staged sidecar without a raw value. Keep it
 * invisible to iteration, but collect it for namespace clear so retry failures
 * cannot permanently consume the database's space. */
static int orphan_metadata_key(const char *prefix, const struct fdb_kv *kv,
                                char out[H2_BK_PREF_KEY_MAX]) {
  if (strncmp(kv->name, "$h2t.", 5))
    return H2_PAL_ERR_NOT_FOUND;
  uint8_t record[TYPE_V2_SIZE];
  size_t offset;
  if (kv->value_len == TYPE_V1_SIZE)
    offset = 24u;
  else if (kv->value_len == TYPE_V2_SIZE)
    offset = TYPE_V2_KEY_OFFSET;
  else
    return H2_PAL_ERR_FORMAT;
  struct fdb_blob blob;
  if (fdb_kv_get_blob(&s_pref_database, kv->name,
                      fdb_blob_make(&blob, record, sizeof(record))) != kv->value_len)
    return H2_PAL_ERR_IO;
  if (memcmp(record, offset == 24u ? "H2TYPE1" : "H2TYPE2", 7) ||
      !memchr(record + offset, '\0', H2_BK_PREF_KEY_MAX))
    return H2_PAL_ERR_FORMAT;
  const char *full = (const char *)record + offset;
  size_t prefix_length = strlen(prefix);
  if (strncmp(full, prefix, prefix_length))
    return H2_PAL_ERR_NOT_FOUND;
  char expected[32];
  metadata_key(full, expected);
  if (strcmp(expected, kv->name) || !full[prefix_length])
    return H2_PAL_ERR_FORMAT;
  struct fdb_kv value = {0};
  if (fdb_kv_get_obj(&s_pref_database, full, &value))
    return H2_PAL_ERR_NOT_FOUND;
  snprintf(out, H2_BK_PREF_KEY_MAX, "%s", full + prefix_length);
  return H2_PAL_OK;
}
static int snapshot(h2_pal_pref_namespace_t *base, h2_pal_pref_cursor_t **out,
                    int include_orphans) {
  h2_bk_pref_namespace_t *ns = bk_pref_to_namespace(base);
  if (!ns || !out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_pal_pref_cursor_t *cursor = os_zalloc(sizeof(*cursor));
  if (!cursor)
    return H2_PAL_ERR_NO_MEMORY;
  cursor->owner = base;
  char prefix[sizeof(ns->name_space) + 1];
  snprintf(prefix, sizeof(prefix), "%s.", ns->name_space);
  size_t len = strlen(prefix);
  struct fdb_kv_iterator iterator;
  fdb_kv_iterator_init(&iterator);
  int rc = H2_PAL_OK;
  while (fdb_kv_iterate(&s_pref_database, &iterator)) {
    struct fdb_kv *kv = &iterator.curr_kv;
    char orphan_key[H2_BK_PREF_KEY_MAX];
    int orphan = strncmp(kv->name, prefix, len) != 0;
    if (orphan) {
      if (!include_orphans)
        continue;
      rc = orphan_metadata_key(prefix, kv, orphan_key);
      if (rc == H2_PAL_ERR_NOT_FOUND) {
        rc = H2_PAL_OK;
        continue;
      }
      if (rc)
        break;
    }
    if (cursor->count >= SIZE_MAX / sizeof(*cursor->entries) - 1u) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
    void *entries = os_realloc(cursor->entries,
                               (cursor->count + 1) * sizeof(*cursor->entries));
    if (!entries) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
    cursor->entries = entries;
    pref_snapshot_entry_t *entry = &cursor->entries[cursor->count];
    snprintf(entry->key, sizeof(entry->key), "%s", orphan ? orphan_key : kv->name + len);
    entry->length = orphan ? 0u : kv->value_len;
    entry->type = H2_PAL_PREF_ENTRY_UNKNOWN;
    if (!orphan)
      rc = value_type(ns, entry->key, &entry->type);
    if (rc)
      break;
    ++cursor->count;
  }
  if (rc) {
    os_free(cursor->entries);
    os_free(cursor);
    return rc;
  }
  *out = cursor;
  return H2_PAL_OK;
}
static int pref_iterate_close(h2_pal_pref_namespace_t *base,
                              h2_pal_pref_cursor_t **cursor) {
  if (!base || !cursor)
    return H2_PAL_ERR_INVALID_ARG;
  if (*cursor && (*cursor)->owner != base)
    return H2_PAL_ERR_INVALID_ARG;
  if (*cursor) {
    os_free((*cursor)->entries);
    os_free(*cursor);
    *cursor = NULL;
  }
  return H2_PAL_OK;
}
static int pref_iterate(h2_pal_pref_namespace_t *base,
                        h2_pal_pref_cursor_t **cursor,
                        h2_pal_pref_entry_t *out) {
  if (!base || !cursor || !out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int rc = storage_lock();
  if (rc)
    return rc;
  if (!*cursor)
    rc = snapshot(base, cursor, 0);
  if (rc)
    return storage_unlock(rc);
  if ((*cursor)->owner != base)
    return storage_unlock(H2_PAL_ERR_INVALID_ARG);
  if ((*cursor)->index == (*cursor)->count)
    return storage_unlock(H2_PAL_ERR_NOT_FOUND);
  const pref_snapshot_entry_t *entry = &(*cursor)->entries[(*cursor)->index++];
  out->key = entry->key;
  out->type = entry->type;
  out->value_size = entry->length;
  return storage_unlock(H2_PAL_OK);
}
static int pref_clear(h2_pal_pref_namespace_t *base) {
  int rc = bk_pref_require_writable(bk_pref_to_namespace(base));
  if (rc)
    return rc;
  rc = storage_lock();
  if (rc)
    return rc;
  h2_pal_pref_cursor_t *cursor = NULL;
  rc = snapshot(base, &cursor, 1);
  if (!rc)
    for (size_t i = 0; i < cursor->count; ++i) {
      rc = remove_value(base, cursor->entries[i].key);
      if (rc == H2_PAL_ERR_NOT_FOUND)
        rc = H2_PAL_OK;
      if (rc)
        break;
    }
  if (cursor)
    pref_iterate_close(base, &cursor);
  return storage_unlock(rc);
}

static int bk_pref_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    h2_bk_pref_namespace_t *ns = NULL;
    int written;
    int rc;

    (void)user;
    if (name_space == NULL || out_namespace == NULL ||
        (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
            mode != H2_PAL_PREF_OPEN_READ_WRITE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_namespace = NULL;
    /* The root and descendants belong exclusively to provider metadata. */
    if (!strncmp(name_space, "$h2t", 4) &&
        (name_space[4] == '\0' || name_space[4] == '.')) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bk_pref_init_database();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    /* Boot workers can hold more than four namespaces concurrently. Each
     * handle owns its state until close; storage operations remain serialized
     * by the database/transaction mutexes. */
    ns = os_zalloc(sizeof(*ns));
    if (ns == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    written = snprintf(ns->name_space, sizeof(ns->name_space), "%s", name_space);
    if (written < 0 || (size_t)written >= sizeof(ns->name_space)) {
        (void)bk_pref_close(&ns->base);
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(&ns->base, 0, sizeof(ns->base));
    ns->mode = mode;
    ns->base.close = bk_pref_close;
    ns->base.get_blob = typed_get_blob;
    ns->base.set_blob = typed_set_blob;
    ns->base.get_string = typed_get_string;
    ns->base.set_string = typed_set_string;
    ns->base.get_u32 = typed_get_u32;
    ns->base.set_u32 = typed_set_u32;
    ns->base.get_i32 = typed_get_i32;
    ns->base.set_i32 = typed_set_i32;
    ns->base.get_bool = typed_get_bool;
    ns->base.set_bool = typed_set_bool;
    ns->base.remove = typed_remove;
    ns->base.commit = bk_pref_commit;
    ns->base.clear = pref_clear;
    ns->base.iterate = pref_iterate;
    ns->base.iterate_close = pref_iterate_close;
    *out_namespace = &ns->base;
    return H2_PAL_OK;
}

static const h2_pal_pref_vtable_t s_pref_vtable = {
    .open = bk_pref_open,
};

static const h2_pal_pref_api_t s_pref_api = {
    .user = NULL,
    .vtable = &s_pref_vtable,
};

const h2_pal_pref_api_t *h2_bk_platform_pref_api(void) {
    return &s_pref_api;
}
