#include "h2_esp_platform_littlefs_io.h"

#include "h2_esp_platform_safe_call.h"

#include "esp_attr.h"

#include <stdlib.h>
#include <string.h>

#define H2_ESP_PREF_IO_BASE_PATH_MAX 192u
#define H2_ESP_PREF_IO_NAMESPACE_MAX 64u
#define H2_ESP_PREF_IO_KEY_MAX 96u
#define H2_ESP_PREF_IO_MARKER_MAX 32u
#define H2_ESP_PREF_IO_MARKER_VALUE_MAX 15u
#define H2_ESP_PREF_IO_VALUE_MAX (16u * 1024u)
#define H2_ESP_PREF_IO_STACK_DEPTH 4096u

typedef enum h2_esp_pref_io_op {
    H2_ESP_PREF_IO_PREPARE = 1,
    H2_ESP_PREF_IO_GET,
    H2_ESP_PREF_IO_SET,
    H2_ESP_PREF_IO_REMOVE,
    H2_ESP_PREF_IO_CLEAR,
    H2_ESP_PREF_IO_LIST,
    H2_ESP_PREF_IO_WRITE_MARKER,
    H2_ESP_PREF_IO_READ_MARKER,
} h2_esp_pref_io_op_t;

typedef struct h2_esp_pref_io_call {
    h2_esp_pref_io_op_t op;
    char base_path[H2_ESP_PREF_IO_BASE_PATH_MAX];
    size_t committed_budget;
    char name_space[H2_ESP_PREF_IO_NAMESPACE_MAX + 1u];
    char key[H2_ESP_PREF_IO_KEY_MAX + 1u];
    char marker[H2_ESP_PREF_IO_MARKER_MAX + 1u];
    char marker_value[H2_ESP_PREF_IO_MARKER_VALUE_MAX + 1u];
    h2_pal_pref_entry_type_t type;
    uint8_t *scratch;
    size_t value_size;
    h2_esp_pref_store_entry_t *entries;
    size_t count;
    int result;
} h2_esp_pref_io_call_t;

static int copy_text(char *out, size_t out_size, const char *text) {
    size_t length;
    if (out == NULL || out_size == 0u || text == NULL) return H2_PAL_ERR_INVALID_ARG;
    length = strlen(text);
    if (length >= out_size) return H2_PAL_ERR_INVALID_ARG;
    memcpy(out, text, length + 1u);
    return H2_PAL_OK;
}

static int init_call(h2_esp_pref_io_call_t *call,
                     const h2_esp_pref_store_t *store,
                     h2_esp_pref_io_op_t op) {
    if (call == NULL || store == NULL || store->base_path == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    memset(call, 0, sizeof(*call));
    call->op = op;
    call->committed_budget = store->committed_budget;
    return copy_text(call->base_path, sizeof(call->base_path), store->base_path);
}

static void IRAM_ATTR pref_io_callback(void *context) {
    h2_esp_pref_io_call_t *call = (h2_esp_pref_io_call_t *)context;
    h2_esp_pref_store_t store = {
        .base_path = call->base_path,
        .committed_budget = call->committed_budget,
    };
    if (call->op == H2_ESP_PREF_IO_PREPARE) {
        call->result = h2_esp_pref_store_prepare(&store);
    } else if (call->op == H2_ESP_PREF_IO_GET) {
        uint8_t *value = NULL;
        size_t value_size = 0u;
        call->result = h2_esp_pref_store_get(
            &store, call->name_space, call->key, call->type, &value,
            &value_size);
        if (call->result == H2_PAL_OK) {
            if (value_size > H2_ESP_PREF_IO_VALUE_MAX) {
                call->result = H2_PAL_ERR_IO;
            } else if (value_size > 0u) {
                memcpy(call->scratch, value, value_size);
            }
            call->value_size = value_size;
        }
        free(value);
    } else if (call->op == H2_ESP_PREF_IO_SET) {
        call->result = h2_esp_pref_store_set(
            &store, call->name_space, call->key, call->type, call->scratch,
            call->value_size);
    } else if (call->op == H2_ESP_PREF_IO_REMOVE) {
        call->result = h2_esp_pref_store_remove(&store, call->name_space,
                                                call->key);
    } else if (call->op == H2_ESP_PREF_IO_CLEAR) {
        call->result = h2_esp_pref_store_clear(&store, call->name_space);
    } else if (call->op == H2_ESP_PREF_IO_LIST) {
        call->result = h2_esp_pref_store_list(
            &store, call->name_space, &call->entries, &call->count);
    } else if (call->op == H2_ESP_PREF_IO_WRITE_MARKER) {
        call->result = h2_esp_pref_store_write_marker(
            &store, call->marker, call->marker_value);
    } else if (call->op == H2_ESP_PREF_IO_READ_MARKER) {
        call->result = h2_esp_pref_store_read_marker(
            &store, call->marker, call->marker_value,
            sizeof(call->marker_value));
    } else {
        call->result = H2_PAL_ERR_INVALID_ARG;
    }
}

static int run_call(h2_esp_pref_io_call_t *call) {
    int rc = h2_esp_platform_safe_call(pref_io_callback, call, sizeof(*call),
                                       H2_ESP_PREF_IO_STACK_DEPTH);
    return rc == H2_PAL_OK ? call->result : rc;
}

int h2_esp_pref_io_prepare(const h2_esp_pref_store_t *store) {
    h2_esp_pref_io_call_t call;
    int rc = init_call(&call, store, H2_ESP_PREF_IO_PREPARE);
    return rc == H2_PAL_OK ? run_call(&call) : rc;
}

int h2_esp_pref_io_get(const h2_esp_pref_store_t *store,
                       const char *name_space, const char *key,
                       h2_pal_pref_entry_type_t expected_type,
                       uint8_t **out_value, size_t *out_value_size) {
    h2_esp_pref_io_call_t call;
    uint8_t *scratch = NULL;
    size_t capacity = 0u;
    uint8_t *copy;
    int rc;
    if (out_value == NULL || out_value_size == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_value = NULL;
    *out_value_size = 0u;
    rc = init_call(&call, store, H2_ESP_PREF_IO_GET);
    if (rc == H2_PAL_OK) rc = copy_text(call.name_space, sizeof(call.name_space), name_space);
    if (rc == H2_PAL_OK) rc = copy_text(call.key, sizeof(call.key), key);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_platform_safe_io_acquire(&scratch, &capacity);
    if (rc != H2_PAL_OK) return rc;
    if (capacity < H2_ESP_PREF_IO_VALUE_MAX) {
        h2_esp_platform_safe_io_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    call.type = expected_type;
    call.scratch = scratch;
    rc = run_call(&call);
    if (rc == H2_PAL_OK) {
        copy = (uint8_t *)malloc(call.value_size == 0u ? 1u : call.value_size);
        if (copy == NULL) rc = H2_PAL_ERR_NO_MEMORY;
        else if (call.value_size > 0u) memcpy(copy, scratch, call.value_size);
        if (rc == H2_PAL_OK) {
            *out_value = copy;
            *out_value_size = call.value_size;
        }
    }
    h2_esp_platform_safe_io_release();
    return rc;
}

int h2_esp_pref_io_set(const h2_esp_pref_store_t *store,
                       const char *name_space, const char *key,
                       h2_pal_pref_entry_type_t type, const void *value,
                       size_t value_size) {
    h2_esp_pref_io_call_t call;
    uint8_t *scratch = NULL;
    size_t capacity = 0u;
    int rc = init_call(&call, store, H2_ESP_PREF_IO_SET);
    if (rc == H2_PAL_OK) rc = copy_text(call.name_space, sizeof(call.name_space), name_space);
    if (rc == H2_PAL_OK) rc = copy_text(call.key, sizeof(call.key), key);
    if (rc != H2_PAL_OK || (value == NULL && value_size != 0u) ||
        value_size > H2_ESP_PREF_IO_VALUE_MAX)
        return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
    rc = h2_esp_platform_safe_io_acquire(&scratch, &capacity);
    if (rc != H2_PAL_OK) return rc;
    if (capacity < value_size) {
        h2_esp_platform_safe_io_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (value_size > 0u) memcpy(scratch, value, value_size);
    call.type = type;
    call.scratch = scratch;
    call.value_size = value_size;
    rc = run_call(&call);
    h2_esp_platform_safe_io_release();
    return rc;
}

static int run_names(const h2_esp_pref_store_t *store, h2_esp_pref_io_op_t op,
                     const char *name_space, const char *key,
                     h2_esp_pref_io_call_t *call) {
    int rc = init_call(call, store, op);
    if (rc == H2_PAL_OK) rc = copy_text(call->name_space, sizeof(call->name_space), name_space);
    if (rc == H2_PAL_OK && key != NULL) rc = copy_text(call->key, sizeof(call->key), key);
    return rc == H2_PAL_OK ? run_call(call) : rc;
}

int h2_esp_pref_io_remove(const h2_esp_pref_store_t *store,
                          const char *name_space, const char *key) {
    h2_esp_pref_io_call_t call;
    return run_names(store, H2_ESP_PREF_IO_REMOVE, name_space, key, &call);
}

int h2_esp_pref_io_clear(const h2_esp_pref_store_t *store,
                         const char *name_space) {
    h2_esp_pref_io_call_t call;
    return run_names(store, H2_ESP_PREF_IO_CLEAR, name_space, NULL, &call);
}

int h2_esp_pref_io_list(const h2_esp_pref_store_t *store,
                        const char *name_space,
                        h2_esp_pref_store_entry_t **out_entries,
                        size_t *out_count) {
    h2_esp_pref_io_call_t call;
    int rc;
    if (out_entries == NULL || out_count == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_entries = NULL;
    *out_count = 0u;
    rc = run_names(store, H2_ESP_PREF_IO_LIST, name_space, NULL, &call);
    if (rc == H2_PAL_OK) {
        *out_entries = call.entries;
        *out_count = call.count;
    }
    return rc;
}

int h2_esp_pref_io_write_marker(const h2_esp_pref_store_t *store,
                                const char *marker, const char *value) {
    h2_esp_pref_io_call_t call;
    int rc = init_call(&call, store, H2_ESP_PREF_IO_WRITE_MARKER);
    if (rc == H2_PAL_OK) rc = copy_text(call.marker, sizeof(call.marker), marker);
    if (rc == H2_PAL_OK) rc = copy_text(call.marker_value, sizeof(call.marker_value), value);
    return rc == H2_PAL_OK ? run_call(&call) : rc;
}

int h2_esp_pref_io_read_marker(const h2_esp_pref_store_t *store,
                               const char *marker, char *out_value,
                               size_t out_value_size) {
    h2_esp_pref_io_call_t call;
    int rc;
    if (out_value == NULL || out_value_size == 0u) return H2_PAL_ERR_INVALID_ARG;
    rc = init_call(&call, store, H2_ESP_PREF_IO_READ_MARKER);
    if (rc == H2_PAL_OK) rc = copy_text(call.marker, sizeof(call.marker), marker);
    if (rc == H2_PAL_OK) rc = run_call(&call);
    if (rc == H2_PAL_OK) rc = copy_text(out_value, out_value_size, call.marker_value);
    return rc;
}
