#include "h2_bk_platform_core.h"

#include "easyflash.h"
#include "flashdb.h"
#include "os/os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_PREF_KEY_MAX 32u
#define H2_BK_PREF_OPEN_MAX 4u

typedef struct h2_bk_pref_namespace {
    h2_pal_pref_namespace_t base;
    char name_space[16];
    h2_pal_pref_open_mode_t mode;
    int in_use;
} h2_bk_pref_namespace_t;

static h2_bk_pref_namespace_t s_pref_namespaces[H2_BK_PREF_OPEN_MAX];
static struct fdb_kvdb s_pref_database;
static beken_mutex_t s_pref_database_mutex;
static beken_mutex_t s_pref_pool_mutex;
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
    int rc;

    if (pref_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bk_pref_lock_pool();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    pref_ns->in_use = 0;
    bk_pref_unlock_pool();
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

static int bk_pref_set_string(h2_pal_pref_namespace_t *base, const char *key, const char *value) {
    if (value == NULL || value[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return bk_pref_set_blob(base, key, value, strlen(value));
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

static int bk_pref_set_u32(h2_pal_pref_namespace_t *base, const char *key, uint32_t value) {
    return bk_pref_set_blob(base, key, &value, sizeof(value));
}

static int bk_pref_get_i32(h2_pal_pref_namespace_t *base, const char *key, int32_t *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0;
    return bk_pref_get_fixed(base, key, out_value, sizeof(*out_value));
}

static int bk_pref_set_i32(h2_pal_pref_namespace_t *base, const char *key, int32_t value) {
    return bk_pref_set_blob(base, key, &value, sizeof(value));
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

static int bk_pref_set_bool(h2_pal_pref_namespace_t *base, const char *key, int value) {
    uint8_t stored = value ? 1u : 0u;
    return bk_pref_set_blob(base, key, &stored, sizeof(stored));
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
    rc = bk_pref_init_database();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bk_pref_lock_pool();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < H2_BK_PREF_OPEN_MAX; ++i) {
        if (!s_pref_namespaces[i].in_use) {
            ns = &s_pref_namespaces[i];
            ns->in_use = 1;
            break;
        }
    }
    bk_pref_unlock_pool();
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
    ns->base.get_blob = bk_pref_get_blob;
    ns->base.set_blob = bk_pref_set_blob;
    ns->base.get_string = bk_pref_get_string;
    ns->base.set_string = bk_pref_set_string;
    ns->base.get_u32 = bk_pref_get_u32;
    ns->base.set_u32 = bk_pref_set_u32;
    ns->base.get_i32 = bk_pref_get_i32;
    ns->base.set_i32 = bk_pref_set_i32;
    ns->base.get_bool = bk_pref_get_bool;
    ns->base.set_bool = bk_pref_set_bool;
    ns->base.remove = bk_pref_remove;
    ns->base.commit = bk_pref_commit;
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
