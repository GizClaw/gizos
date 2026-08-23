#include "h2_sqlite.h"

#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_SQLITE_PREF_PATH_MAX 1024u
#define H2_SQLITE_PREF_NAMESPACE_MAX 64u
#define H2_SQLITE_PREF_KEY_MAX 96u
#define H2_SQLITE_PREF_VALUE_MAX (16u * 1024u)

typedef struct h2_sqlite_pref_namespace {
    h2_pal_pref_namespace_t base;
    sqlite3 *db;
    char *name_space;
    h2_pal_pref_open_mode_t mode;
    int transaction_open;
} h2_sqlite_pref_namespace_t;

struct h2_pal_pref_cursor {
    sqlite3_stmt *stmt;
};

struct h2_sqlite {
    char path[H2_SQLITE_PREF_PATH_MAX];
    int path_status;
    h2_pal_pref_api_t api;
};

static h2_sqlite_pref_namespace_t *to_sqlite_namespace(h2_pal_pref_namespace_t *store) {
    if (store == NULL) {
        return NULL;
    }
    return (h2_sqlite_pref_namespace_t *)store->user;
}

static size_t pref_strnlen(const char *text, size_t max_len) {
    size_t len = 0u;
    if (text == NULL) {
        return 0u;
    }
    while (len < max_len && text[len] != '\0') {
        ++len;
    }
    return len;
}

static int validate_text_len(const char *text, size_t max_len) {
    size_t len;
    if (text == NULL || text[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = pref_strnlen(text, max_len + 1u);
    return len > 0u && len <= max_len ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
}

static int validate_namespace(const char *name_space) {
    return validate_text_len(name_space, H2_SQLITE_PREF_NAMESPACE_MAX);
}

static int validate_key(const char *key) {
    return validate_text_len(key, H2_SQLITE_PREF_KEY_MAX);
}

static char *copy_text(const char *text) {
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1u);
    return copy;
}

static int sqlite_result(int rc) {
    switch (rc) {
    case SQLITE_OK:
    case SQLITE_DONE:
    case SQLITE_ROW:
        return H2_PAL_OK;
    case SQLITE_NOMEM:
        return H2_PAL_ERR_NO_MEMORY;
    case SQLITE_FULL:
        return H2_PAL_ERR_NO_SPACE;
    case SQLITE_READONLY:
        return H2_PAL_ERR_INVALID_STATE;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return H2_PAL_ERR_WOULD_BLOCK;
    case SQLITE_NOTFOUND:
        return H2_PAL_ERR_NOT_FOUND;
    default:
        return H2_PAL_ERR_IO;
    }
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    sqlite3_free(err);
    return sqlite_result(rc);
}

static int ensure_schema(sqlite3 *db) {
    return exec_sql(
        db,
        "CREATE TABLE IF NOT EXISTS h2_pref ("
        "namespace TEXT NOT NULL,"
        "key TEXT NOT NULL,"
        "type INTEGER NOT NULL,"
        "value BLOB NOT NULL,"
        "updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY(namespace, key)"
        ")");
}

static int begin_transaction(h2_sqlite_pref_namespace_t *store) {
    int rc;
    if (store->mode != H2_PAL_PREF_OPEN_READ_WRITE || store->transaction_open) {
        return H2_PAL_OK;
    }
    rc = exec_sql(store->db, "BEGIN IMMEDIATE");
    if (rc == H2_PAL_OK) {
        store->transaction_open = 1;
    }
    return rc;
}

static int bind_namespace_key(sqlite3_stmt *stmt, const h2_sqlite_pref_namespace_t *store, const char *key) {
    int rc = sqlite3_bind_text(stmt, 1, store->name_space, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        return sqlite_result(rc);
    }
    rc = sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    return sqlite_result(rc);
}

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out_stmt) {
    int rc;
    if (out_stmt == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, out_stmt, NULL);
    return sqlite_result(rc);
}

static int read_value(
    h2_sqlite_pref_namespace_t *store,
    const char *key,
    h2_pal_pref_entry_type_t expected_type,
    sqlite3_stmt **out_stmt,
    const void **out_data,
    size_t *out_len) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int type;

    rc = prepare(
        store->db,
        "SELECT type, value FROM h2_pref WHERE namespace = ?1 AND key = ?2",
        &stmt);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bind_namespace_key(stmt, store, key);
    if (rc != H2_PAL_OK) {
        sqlite3_finalize(stmt);
        return rc;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return sqlite_result(rc);
    }
    type = sqlite3_column_int(stmt, 0);
    if (type != (int)expected_type) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_data = sqlite3_column_blob(stmt, 1);
    *out_len = (size_t)sqlite3_column_bytes(stmt, 1);
    *out_stmt = stmt;
    return H2_PAL_OK;
}

static int write_value(
    h2_sqlite_pref_namespace_t *store,
    const char *key,
    h2_pal_pref_entry_type_t type,
    const void *data,
    size_t data_len) {
    sqlite3_stmt *stmt = NULL;
    static const uint8_t empty_blob = 0u;
    int rc;

    if (store->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = begin_transaction(store);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = prepare(
        store->db,
        "INSERT INTO h2_pref(namespace, key, type, value, updated_at_ms) "
        "VALUES(?1, ?2, ?3, ?4, unixepoch('now') * 1000) "
        "ON CONFLICT(namespace, key) DO UPDATE SET "
        "type = excluded.type, value = excluded.value, updated_at_ms = excluded.updated_at_ms",
        &stmt);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bind_namespace_key(stmt, store, key);
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_bind_int(stmt, 3, (int)type));
    }
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_bind_blob(
            stmt,
            4,
            data_len == 0u ? &empty_blob : data,
            (int)data_len,
            SQLITE_TRANSIENT));
    }
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_step(stmt));
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int sqlite_pref_close(h2_pal_pref_namespace_t *store) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    int rc = H2_PAL_OK;
    if (desktop_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (desktop_ns->transaction_open) {
        rc = exec_sql(desktop_ns->db, "ROLLBACK");
    }
    if (desktop_ns->db != NULL) {
        int close_rc = sqlite3_close_v2(desktop_ns->db);
        if (rc == H2_PAL_OK) {
            rc = sqlite_result(close_rc);
        }
    }
    free(desktop_ns->name_space);
    free(desktop_ns);
    return rc;
}

static int sqlite_pref_get_blob(
    h2_pal_pref_namespace_t *store,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    sqlite3_stmt *stmt = NULL;
    const void *data = NULL;
    size_t len = 0u;
    void *copy;
    int rc;
    if (desktop_ns == NULL || allocator == NULL || validate_key(key) != H2_PAL_OK || out_data == NULL ||
        out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0u;
    rc = read_value(desktop_ns, key, H2_PAL_PREF_ENTRY_BLOB, &stmt, &data, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    copy = h2_pal_mem_alloc(allocator, len == 0u ? 1u : len);
    if (copy == NULL) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (len > 0u) {
        memcpy(copy, data, len);
    }
    sqlite3_finalize(stmt);
    *out_data = copy;
    *out_len = len;
    return H2_PAL_OK;
}

static int sqlite_pref_set_blob(
    h2_pal_pref_namespace_t *store,
    const char *key,
    const void *data,
    size_t data_len) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK || (data == NULL && data_len != 0u) ||
        data_len > H2_SQLITE_PREF_VALUE_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_value(desktop_ns, key, H2_PAL_PREF_ENTRY_BLOB, data, data_len);
}

static int sqlite_pref_get_string(
    h2_pal_pref_namespace_t *store,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char **out_value) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    sqlite3_stmt *stmt = NULL;
    const void *data = NULL;
    size_t len = 0u;
    char *copy;
    int rc;
    if (desktop_ns == NULL || allocator == NULL || validate_key(key) != H2_PAL_OK || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    rc = read_value(desktop_ns, key, H2_PAL_PREF_ENTRY_STRING, &stmt, &data, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    copy = (char *)h2_pal_mem_alloc(allocator, len + 1u);
    if (copy == NULL) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (len > 0u) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    sqlite3_finalize(stmt);
    *out_value = copy;
    return H2_PAL_OK;
}

static int sqlite_pref_set_string(
    h2_pal_pref_namespace_t *store,
    const char *key,
    const char *value) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    size_t len;
    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK || value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = pref_strnlen(value, H2_SQLITE_PREF_VALUE_MAX + 1u);
    if (len > H2_SQLITE_PREF_VALUE_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_value(desktop_ns, key, H2_PAL_PREF_ENTRY_STRING, value, len);
}

static int read_u32_value(
    h2_sqlite_pref_namespace_t *store,
    const char *key,
    h2_pal_pref_entry_type_t type,
    uint32_t *out_value) {
    sqlite3_stmt *stmt = NULL;
    const void *data = NULL;
    size_t len = 0u;
    uint32_t value;
    int rc = read_value(store, key, type, &stmt, &data, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (len != sizeof(value)) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_IO;
    }
    memcpy(&value, data, sizeof(value));
    sqlite3_finalize(stmt);
    *out_value = value;
    return H2_PAL_OK;
}

static int read_i32_value(
    h2_sqlite_pref_namespace_t *store,
    const char *key,
    h2_pal_pref_entry_type_t type,
    int32_t *out_value) {
    sqlite3_stmt *stmt = NULL;
    const void *data = NULL;
    size_t len = 0u;
    int32_t value;
    int rc = read_value(store, key, type, &stmt, &data, &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (len != sizeof(value)) {
        sqlite3_finalize(stmt);
        return H2_PAL_ERR_IO;
    }
    memcpy(&value, data, sizeof(value));
    sqlite3_finalize(stmt);
    *out_value = value;
    return H2_PAL_OK;
}

static int sqlite_pref_get_u32(
    h2_pal_pref_namespace_t *store,
    const char *key,
    uint32_t *out_value) {
    if (to_sqlite_namespace(store) == NULL || validate_key(key) != H2_PAL_OK || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_u32_value(to_sqlite_namespace(store), key, H2_PAL_PREF_ENTRY_U32, out_value);
}

static int sqlite_pref_set_u32(
    h2_pal_pref_namespace_t *store,
    const char *key,
    uint32_t value) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_value(desktop_ns, key, H2_PAL_PREF_ENTRY_U32, &value, sizeof(value));
}

static int sqlite_pref_get_i32(
    h2_pal_pref_namespace_t *store,
    const char *key,
    int32_t *out_value) {
    if (to_sqlite_namespace(store) == NULL || validate_key(key) != H2_PAL_OK || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_i32_value(to_sqlite_namespace(store), key, H2_PAL_PREF_ENTRY_I32, out_value);
}

static int sqlite_pref_set_i32(
    h2_pal_pref_namespace_t *store,
    const char *key,
    int32_t value) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_value(desktop_ns, key, H2_PAL_PREF_ENTRY_I32, &value, sizeof(value));
}

static int sqlite_pref_get_bool(
    h2_pal_pref_namespace_t *store,
    const char *key,
    int *out_value) {
    int32_t value = 0;
    int rc;
    if (to_sqlite_namespace(store) == NULL || validate_key(key) != H2_PAL_OK || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = read_i32_value(to_sqlite_namespace(store), key, H2_PAL_PREF_ENTRY_BOOL, &value);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_value = value != 0;
    return H2_PAL_OK;
}

static int sqlite_pref_set_bool(
    h2_pal_pref_namespace_t *store,
    const char *key,
    int value) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    int32_t stored = value != 0 ? 1 : 0;
    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_value(desktop_ns, key, H2_PAL_PREF_ENTRY_BOOL, &stored, sizeof(stored));
}

static int sqlite_pref_remove(h2_pal_pref_namespace_t *store, const char *key) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (desktop_ns == NULL || validate_key(key) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (desktop_ns->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = begin_transaction(desktop_ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = prepare(desktop_ns->db, "DELETE FROM h2_pref WHERE namespace = ?1 AND key = ?2", &stmt);
    if (rc == H2_PAL_OK) {
        rc = bind_namespace_key(stmt, desktop_ns, key);
    }
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_step(stmt));
    }
    sqlite3_finalize(stmt);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (sqlite3_changes(desktop_ns->db) == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    return H2_PAL_OK;
}

static int sqlite_pref_clear(h2_pal_pref_namespace_t *store) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (desktop_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (desktop_ns->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = begin_transaction(desktop_ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = prepare(desktop_ns->db, "DELETE FROM h2_pref WHERE namespace = ?1", &stmt);
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_bind_text(stmt, 1, desktop_ns->name_space, -1, SQLITE_STATIC));
    }
    if (rc == H2_PAL_OK) {
        rc = sqlite_result(sqlite3_step(stmt));
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int sqlite_pref_commit(h2_pal_pref_namespace_t *store) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    int rc;
    if (desktop_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (desktop_ns->mode != H2_PAL_PREF_OPEN_READ_WRITE) {
        return H2_PAL_OK;
    }
    if (!desktop_ns->transaction_open) {
        return H2_PAL_OK;
    }
    rc = exec_sql(desktop_ns->db, "COMMIT");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    desktop_ns->transaction_open = 0;
    return H2_PAL_OK;
}

static int sqlite_pref_iterate(
    h2_pal_pref_namespace_t *store,
    h2_pal_pref_cursor_t **cursor,
    h2_pal_pref_entry_t *out_entry) {
    h2_sqlite_pref_namespace_t *desktop_ns = to_sqlite_namespace(store);
    h2_pal_pref_cursor_t *current;
    int rc;
    const unsigned char *key;

    if (desktop_ns == NULL || cursor == NULL || out_entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*cursor == NULL) {
        current = (h2_pal_pref_cursor_t *)calloc(1u, sizeof(*current));
        if (current == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        rc = prepare(
            desktop_ns->db,
            "SELECT key, type, length(value) FROM h2_pref WHERE namespace = ?1 ORDER BY key",
            &current->stmt);
        if (rc == H2_PAL_OK) {
            rc = sqlite_result(sqlite3_bind_text(current->stmt, 1, desktop_ns->name_space, -1, SQLITE_STATIC));
        }
        if (rc != H2_PAL_OK) {
            if (current->stmt != NULL) {
                sqlite3_finalize(current->stmt);
            }
            free(current);
            return rc;
        }
        *cursor = current;
    }
    current = *cursor;
    rc = sqlite3_step(current->stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(current->stmt);
        free(current);
        *cursor = NULL;
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(current->stmt);
        free(current);
        *cursor = NULL;
        return sqlite_result(rc);
    }
    memset(out_entry, 0, sizeof(*out_entry));
    key = sqlite3_column_text(current->stmt, 0);
    if (key == NULL) {
        return H2_PAL_ERR_IO;
    }
    out_entry->key = (const char *)key;
    out_entry->type = (h2_pal_pref_entry_type_t)sqlite3_column_int(current->stmt, 1);
    out_entry->value_size = (size_t)sqlite3_column_int64(current->stmt, 2);
    return H2_PAL_OK;
}

static int sqlite_pref_iterate_close(
    h2_pal_pref_namespace_t *store,
    h2_pal_pref_cursor_t **cursor) {
    h2_pal_pref_cursor_t *current;
    (void)store;
    if (cursor == NULL || *cursor == NULL) {
        return H2_PAL_OK;
    }
    current = *cursor;
    if (current->stmt != NULL) {
        sqlite3_finalize(current->stmt);
    }
    free(current);
    *cursor = NULL;
    return H2_PAL_OK;
}

static void sqlite_pref_namespace_init(h2_sqlite_pref_namespace_t *store) {
    store->base.user = store;
    store->base.close = sqlite_pref_close;
    store->base.get_blob = sqlite_pref_get_blob;
    store->base.set_blob = sqlite_pref_set_blob;
    store->base.get_string = sqlite_pref_get_string;
    store->base.set_string = sqlite_pref_set_string;
    store->base.get_u32 = sqlite_pref_get_u32;
    store->base.set_u32 = sqlite_pref_set_u32;
    store->base.get_i32 = sqlite_pref_get_i32;
    store->base.set_i32 = sqlite_pref_set_i32;
    store->base.get_bool = sqlite_pref_get_bool;
    store->base.set_bool = sqlite_pref_set_bool;
    store->base.remove = sqlite_pref_remove;
    store->base.clear = sqlite_pref_clear;
    store->base.commit = sqlite_pref_commit;
    store->base.iterate = sqlite_pref_iterate;
    store->base.iterate_close = sqlite_pref_iterate_close;
}

static int pref_api_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    h2_sqlite_t *provider = (h2_sqlite_t *)user;
    const char *path;
    h2_sqlite_pref_namespace_t *store;
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READONLY;
    int rc;

    if (provider == NULL || out_namespace == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_namespace = NULL;
    rc = validate_namespace(name_space);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (mode != H2_PAL_PREF_OPEN_READ_ONLY && mode != H2_PAL_PREF_OPEN_READ_WRITE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (provider->path_status != H2_PAL_OK) {
        return provider->path_status;
    }
    path = provider->path;
    if (path == NULL || path[0] == '\0') {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (mode == H2_PAL_PREF_OPEN_READ_WRITE) {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }
    rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (db != NULL) {
            sqlite3_close(db);
        }
        return sqlite_result(rc);
    }
    rc = ensure_schema(db);
    if (rc != H2_PAL_OK) {
        sqlite3_close(db);
        return rc;
    }
    store = (h2_sqlite_pref_namespace_t *)calloc(1u, sizeof(*store));
    if (store == NULL) {
        sqlite3_close(db);
        return H2_PAL_ERR_NO_MEMORY;
    }
    sqlite_pref_namespace_init(store);
    store->db = db;
    store->mode = mode;
    store->name_space = copy_text(name_space);
    if (store->name_space == NULL) {
        sqlite3_close(db);
        free(store);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_namespace = &store->base;
    return H2_PAL_OK;
}

static const h2_pal_pref_vtable_t g_pref_vtable = {
    .open = pref_api_open,
};

int h2_sqlite_create(const h2_sqlite_config_t *config,
                     h2_sqlite_t **out_provider) {
    h2_sqlite_t *provider;
    size_t path_len;

    if (config == NULL || out_provider == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_provider = NULL;
    if (config->path == NULL || config->path[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    path_len = strlen(config->path);
    if (path_len >= H2_SQLITE_PREF_PATH_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    provider = (h2_sqlite_t *)calloc(1u, sizeof(*provider));
    if (provider == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(provider->path, config->path, path_len + 1u);
    provider->path_status = H2_PAL_OK;
    provider->api.user = provider;
    provider->api.vtable = &g_pref_vtable;
    *out_provider = provider;
    return H2_PAL_OK;
}

void h2_sqlite_destroy(h2_sqlite_t *provider) {
    free(provider);
}

const h2_pal_pref_api_t *h2_sqlite_pref_api(h2_sqlite_t *provider) {
    return provider == NULL ? NULL : &provider->api;
}
