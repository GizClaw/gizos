#include "h2_esp_platform_core.h"
#include "h2_esp_platform_littlefs_io.h"
#include "h2_esp_platform_pref_store.h"
#include "h2_esp_platform_pref_migration.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_littlefs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

#define H2_ESP_PREF_PARTITION_LABEL "pref"
#define H2_ESP_PREF_BASE_PATH "/h2pref"
#define H2_ESP_PREF_PARTITION_SIZE (256u * 1024u)
#define H2_ESP_PREF_COMMITTED_BUDGET (128u * 1024u)
#define H2_ESP_PREF_NAMESPACE_MAX 64u
#define H2_ESP_PREF_KEY_MAX 96u
#define H2_ESP_PREF_VALUE_MAX (16u * 1024u)
#define H2_ESP_PREF_SAFE_STACK_DEPTH 4096u

typedef enum h2_esp_pref_mount_op {
    H2_ESP_PREF_MOUNT_VALIDATE = 1,
    H2_ESP_PREF_MOUNT_REGISTER,
    H2_ESP_PREF_MOUNT_FORMAT,
    H2_ESP_PREF_MOUNT_CHECK_ERASED,
} h2_esp_pref_mount_op_t;

typedef struct h2_esp_pref_mount_call {
    h2_esp_pref_mount_op_t op;
    esp_err_t result;
    int erased;
} h2_esp_pref_mount_call_t;

typedef struct h2_esp_pref_namespace {
    h2_pal_pref_namespace_t base;
    char name_space[H2_ESP_PREF_NAMESPACE_MAX + 1u];
    h2_pal_pref_open_mode_t mode;
} h2_esp_pref_namespace_t;

struct h2_pal_pref_cursor {
    h2_esp_pref_store_entry_t *entries;
    size_t count;
    size_t index;
};

static h2_esp_pref_store_t s_store = {
    .base_path = H2_ESP_PREF_BASE_PATH,
    .committed_budget = H2_ESP_PREF_COMMITTED_BUDGET,
};
static StaticSemaphore_t s_pref_mutex_storage;
static SemaphoreHandle_t s_pref_mutex;
static portMUX_TYPE s_pref_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_pref_ready;

static SemaphoreHandle_t pref_mutex(void) {
    portENTER_CRITICAL(&s_pref_mutex_init_lock);
    if (s_pref_mutex == NULL) {
        s_pref_mutex = xSemaphoreCreateMutexStatic(&s_pref_mutex_storage);
    }
    portEXIT_CRITICAL(&s_pref_mutex_init_lock);
    return s_pref_mutex;
}

static int pref_lock(void) {
    SemaphoreHandle_t mutex = pref_mutex();
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static void pref_unlock(void) {
    if (s_pref_mutex != NULL) (void)xSemaphoreGive(s_pref_mutex);
}

static void IRAM_ATTR pref_mount_safe_callback(void *context) {
    h2_esp_pref_mount_call_t *call = (h2_esp_pref_mount_call_t *)context;
    if (call->op == H2_ESP_PREF_MOUNT_VALIDATE) {
        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
            H2_ESP_PREF_PARTITION_LABEL);
        if (partition == NULL ||
            partition->size != H2_ESP_PREF_PARTITION_SIZE) {
            call->result = ESP_ERR_NOT_FOUND;
        } else {
            call->result = ESP_OK;
        }
        return;
    }
    if (call->op == H2_ESP_PREF_MOUNT_REGISTER) {
        const esp_vfs_littlefs_conf_t config = {
            .base_path = H2_ESP_PREF_BASE_PATH,
            .partition_label = H2_ESP_PREF_PARTITION_LABEL,
            .format_if_mount_failed = false,
            .dont_mount = false,
        };
        call->result = esp_vfs_littlefs_register(&config);
        return;
    }
    if (call->op == H2_ESP_PREF_MOUNT_FORMAT) {
        call->result = esp_littlefs_format(H2_ESP_PREF_PARTITION_LABEL);
        return;
    }
    if (call->op == H2_ESP_PREF_MOUNT_CHECK_ERASED) {
        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
            H2_ESP_PREF_PARTITION_LABEL);
        uint8_t bytes[256];
        size_t offset;
        call->erased = 0;
        if (partition == NULL || partition->size != H2_ESP_PREF_PARTITION_SIZE) {
            call->result = ESP_ERR_NOT_FOUND;
            return;
        }
        for (offset = 0u; offset < partition->size; offset += sizeof(bytes)) {
            size_t length = partition->size - offset;
            size_t index;
            if (length > sizeof(bytes)) length = sizeof(bytes);
            call->result = esp_partition_read(partition, offset, bytes, length);
            if (call->result != ESP_OK) return;
            for (index = 0u; index < length; ++index) {
                if (bytes[index] != 0xffu) {
                    call->result = ESP_OK;
                    return;
                }
            }
        }
        call->erased = 1;
        call->result = ESP_OK;
        return;
    }
    call->result = ESP_ERR_INVALID_ARG;
}

static esp_err_t pref_mount_run(h2_esp_pref_mount_call_t *call) {
    h2_pal_result_t rc = h2_esp_platform_safe_call(
        pref_mount_safe_callback, call, sizeof(*call),
        H2_ESP_PREF_SAFE_STACK_DEPTH);
    return rc == H2_PAL_OK ? call->result : ESP_ERR_NO_MEM;
}

static int pref_ensure_ready(void) {
    h2_esp_pref_mount_call_t call = {.op = H2_ESP_PREF_MOUNT_VALIDATE};
    esp_err_t result;
    int rc;
    if (s_pref_ready) return H2_PAL_OK;
    result = pref_mount_run(&call);
    if (result != ESP_OK) return H2_PAL_ERR_IO;
    memset(&call, 0, sizeof(call));
    call.op = H2_ESP_PREF_MOUNT_REGISTER;
    result = pref_mount_run(&call);
    if (result != ESP_OK) {
        memset(&call, 0, sizeof(call));
        call.op = H2_ESP_PREF_MOUNT_CHECK_ERASED;
        result = pref_mount_run(&call);
        if (result != ESP_OK || !call.erased) return H2_PAL_ERR_IO;
        memset(&call, 0, sizeof(call));
        call.op = H2_ESP_PREF_MOUNT_FORMAT;
        if (pref_mount_run(&call) != ESP_OK) return H2_PAL_ERR_IO;
        memset(&call, 0, sizeof(call));
        call.op = H2_ESP_PREF_MOUNT_REGISTER;
        if (pref_mount_run(&call) != ESP_OK) return H2_PAL_ERR_IO;
    }
    rc = h2_esp_pref_io_prepare(&s_store);
    if (rc == H2_PAL_OK) rc = h2_esp_pref_migration_prepare(&s_store);
    if (rc == H2_PAL_OK) s_pref_ready = 1;
    return rc;
}

h2_pal_result_t h2_esp_platform_pref_finalize_migration(void) {
    int rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = pref_ensure_ready();
    if (rc == H2_PAL_OK) rc = h2_esp_pref_migration_finalize();
    pref_unlock();
    return rc;
}

static size_t bounded_strlen(const char *text, size_t limit) {
    size_t length = 0u;
    if (text == NULL) return 0u;
    while (length <= limit && text[length] != '\0') ++length;
    return length;
}

static int validate_name(const char *text, size_t limit) {
    size_t length = bounded_strlen(text, limit);
    return length > 0u && length <= limit ? H2_PAL_OK
                                          : H2_PAL_ERR_INVALID_ARG;
}

static h2_esp_pref_namespace_t *to_namespace(h2_pal_pref_namespace_t *raw) {
    return raw == NULL ? NULL : (h2_esp_pref_namespace_t *)raw->user;
}

static int ensure_rw(h2_esp_pref_namespace_t *ns) {
    if (ns == NULL) return H2_PAL_ERR_INVALID_ARG;
    return ns->mode == H2_PAL_PREF_OPEN_READ_WRITE ? H2_PAL_OK
                                                    : H2_PAL_ERR_INVALID_STATE;
}

static int pref_read(h2_esp_pref_namespace_t *ns, const char *key,
                     h2_pal_pref_entry_type_t type, uint8_t **out_value,
                     size_t *out_size) {
    int rc;
    if (ns == NULL || validate_name(key, H2_ESP_PREF_KEY_MAX) != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_ARG;
    rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_pref_io_get(&s_store, ns->name_space, key, type,
                            out_value, out_size);
    pref_unlock();
    return rc;
}

static int pref_write(h2_esp_pref_namespace_t *ns, const char *key,
                      h2_pal_pref_entry_type_t type, const void *value,
                      size_t size) {
    int rc = ensure_rw(ns);
    if (rc != H2_PAL_OK) return rc;
    if (validate_name(key, H2_ESP_PREF_KEY_MAX) != H2_PAL_OK ||
        (value == NULL && size != 0u) || size > H2_ESP_PREF_VALUE_MAX)
        return H2_PAL_ERR_INVALID_ARG;
    rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_pref_io_set(&s_store, ns->name_space, key, type, value, size);
    pref_unlock();
    return rc;
}

static int esp_pref_close(h2_pal_pref_namespace_t *raw) {
    h2_esp_pref_namespace_t *ns = to_namespace(raw);
    if (ns == NULL) return H2_PAL_ERR_INVALID_ARG;
    free(ns);
    return H2_PAL_OK;
}

static int esp_pref_get_blob(h2_pal_pref_namespace_t *raw,
                             const h2_pal_mem_api_t *allocator,
                             const char *key, void **out_data,
                             size_t *out_len) {
    uint8_t *value = NULL;
    size_t size = 0u;
    void *copy;
    int rc;
    if (allocator == NULL || out_data == NULL || out_len == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_data = NULL; *out_len = 0u;
    rc = pref_read(to_namespace(raw), key, H2_PAL_PREF_ENTRY_BLOB, &value, &size);
    if (rc != H2_PAL_OK) return rc;
    copy = h2_pal_mem_alloc(allocator, size == 0u ? 1u : size);
    if (copy == NULL) { free(value); return H2_PAL_ERR_NO_MEMORY; }
    if (size > 0u) memcpy(copy, value, size);
    free(value); *out_data = copy; *out_len = size;
    return H2_PAL_OK;
}

static int esp_pref_set_blob(h2_pal_pref_namespace_t *raw, const char *key,
                             const void *data, size_t len) {
    return pref_write(to_namespace(raw), key, H2_PAL_PREF_ENTRY_BLOB, data, len);
}

static int esp_pref_get_string(h2_pal_pref_namespace_t *raw,
                               const h2_pal_mem_api_t *allocator,
                               const char *key, char **out_value) {
    uint8_t *value = NULL;
    size_t size = 0u;
    char *copy;
    int rc;
    if (allocator == NULL || out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_value = NULL;
    rc = pref_read(to_namespace(raw), key, H2_PAL_PREF_ENTRY_STRING, &value, &size);
    if (rc != H2_PAL_OK) return rc;
    copy = (char *)h2_pal_mem_alloc(allocator, size + 1u);
    if (copy == NULL) { free(value); return H2_PAL_ERR_NO_MEMORY; }
    if (size > 0u) memcpy(copy, value, size);
    copy[size] = '\0'; free(value); *out_value = copy;
    return H2_PAL_OK;
}

static int esp_pref_set_string(h2_pal_pref_namespace_t *raw, const char *key,
                               const char *value) {
    size_t size = bounded_strlen(value, H2_ESP_PREF_VALUE_MAX);
    if (value == NULL || size > H2_ESP_PREF_VALUE_MAX) return H2_PAL_ERR_INVALID_ARG;
    return pref_write(to_namespace(raw), key, H2_PAL_PREF_ENTRY_STRING, value, size);
}

static int pref_get_u32_value(h2_pal_pref_namespace_t *raw, const char *key,
                              h2_pal_pref_entry_type_t type, uint32_t *out_value) {
    uint8_t *value = NULL;
    size_t size = 0u;
    int rc;
    if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = pref_read(to_namespace(raw), key, type, &value, &size);
    if (rc != H2_PAL_OK) return rc;
    if (size != 4u) { free(value); return H2_PAL_ERR_IO; }
    *out_value = (uint32_t)value[0] | ((uint32_t)value[1] << 8u) |
                 ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
    free(value); return H2_PAL_OK;
}

static int pref_set_u32_value(h2_pal_pref_namespace_t *raw, const char *key,
                              h2_pal_pref_entry_type_t type, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8u),
                        (uint8_t)(value >> 16u), (uint8_t)(value >> 24u)};
    return pref_write(to_namespace(raw), key, type, bytes, sizeof(bytes));
}

static int esp_pref_get_u32(h2_pal_pref_namespace_t *raw, const char *key,
                            uint32_t *out_value) {
    return pref_get_u32_value(raw, key, H2_PAL_PREF_ENTRY_U32, out_value);
}

static int esp_pref_set_u32(h2_pal_pref_namespace_t *raw, const char *key,
                            uint32_t value) {
    return pref_set_u32_value(raw, key, H2_PAL_PREF_ENTRY_U32, value);
}

static int esp_pref_get_i32(h2_pal_pref_namespace_t *raw, const char *key,
                            int32_t *out_value) {
    uint32_t value;
    int rc;
    if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = pref_get_u32_value(raw, key, H2_PAL_PREF_ENTRY_I32, &value);
    if (rc == H2_PAL_OK) *out_value = (int32_t)value;
    return rc;
}

static int esp_pref_set_i32(h2_pal_pref_namespace_t *raw, const char *key,
                            int32_t value) {
    return pref_set_u32_value(raw, key, H2_PAL_PREF_ENTRY_I32, (uint32_t)value);
}

static int esp_pref_get_bool(h2_pal_pref_namespace_t *raw, const char *key,
                             int *out_value) {
    uint8_t *value = NULL;
    size_t size = 0u;
    int rc;
    if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = pref_read(to_namespace(raw), key, H2_PAL_PREF_ENTRY_BOOL, &value, &size);
    if (rc != H2_PAL_OK) return rc;
    if (size != 1u || value[0] > 1u) { free(value); return H2_PAL_ERR_IO; }
    *out_value = value[0] != 0u; free(value); return H2_PAL_OK;
}

static int esp_pref_set_bool(h2_pal_pref_namespace_t *raw, const char *key,
                             int value) {
    uint8_t stored = value != 0 ? 1u : 0u;
    return pref_write(to_namespace(raw), key, H2_PAL_PREF_ENTRY_BOOL,
                      &stored, sizeof(stored));
}

static int esp_pref_remove(h2_pal_pref_namespace_t *raw, const char *key) {
    h2_esp_pref_namespace_t *ns = to_namespace(raw);
    int rc = ensure_rw(ns);
    if (rc != H2_PAL_OK || validate_name(key, H2_ESP_PREF_KEY_MAX) != H2_PAL_OK)
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_ARG;
    rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_pref_io_remove(&s_store, ns->name_space, key);
    pref_unlock(); return rc;
}

static int esp_pref_clear(h2_pal_pref_namespace_t *raw) {
    h2_esp_pref_namespace_t *ns = to_namespace(raw);
    int rc = ensure_rw(ns);
    if (rc != H2_PAL_OK) return rc;
    rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_pref_io_clear(&s_store, ns->name_space);
    pref_unlock(); return rc;
}

static int esp_pref_commit(h2_pal_pref_namespace_t *raw) {
    return to_namespace(raw) == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK;
}

static int esp_pref_iterate(h2_pal_pref_namespace_t *raw,
                            h2_pal_pref_cursor_t **cursor,
                            h2_pal_pref_entry_t *out_entry) {
    h2_esp_pref_namespace_t *ns = to_namespace(raw);
    h2_pal_pref_cursor_t *current;
    int rc;
    if (ns == NULL || cursor == NULL || out_entry == NULL) return H2_PAL_ERR_INVALID_ARG;
    current = *cursor;
    if (current == NULL) {
        current = (h2_pal_pref_cursor_t *)calloc(1u, sizeof(*current));
        if (current == NULL) return H2_PAL_ERR_NO_MEMORY;
        rc = pref_lock();
        if (rc == H2_PAL_OK) {
            rc = h2_esp_pref_io_list(&s_store, ns->name_space,
                                     &current->entries, &current->count);
            pref_unlock();
        }
        if (rc != H2_PAL_OK) { free(current); return rc; }
        *cursor = current;
    }
    if (current->index >= current->count) {
        free(current->entries); free(current); *cursor = NULL;
        return H2_PAL_ERR_NOT_FOUND;
    }
    out_entry->key = current->entries[current->index].key;
    out_entry->type = current->entries[current->index].type;
    out_entry->value_size = current->entries[current->index].value_size;
    current->index++;
    return H2_PAL_OK;
}

static int esp_pref_iterate_close(h2_pal_pref_namespace_t *raw,
                                  h2_pal_pref_cursor_t **cursor) {
    (void)raw;
    if (cursor == NULL || *cursor == NULL) return H2_PAL_OK;
    free((*cursor)->entries); free(*cursor); *cursor = NULL;
    return H2_PAL_OK;
}

static void namespace_init(h2_esp_pref_namespace_t *ns) {
    ns->base.user = ns;
    ns->base.close = esp_pref_close;
    ns->base.get_blob = esp_pref_get_blob;
    ns->base.set_blob = esp_pref_set_blob;
    ns->base.get_string = esp_pref_get_string;
    ns->base.set_string = esp_pref_set_string;
    ns->base.get_u32 = esp_pref_get_u32;
    ns->base.set_u32 = esp_pref_set_u32;
    ns->base.get_i32 = esp_pref_get_i32;
    ns->base.set_i32 = esp_pref_set_i32;
    ns->base.get_bool = esp_pref_get_bool;
    ns->base.set_bool = esp_pref_set_bool;
    ns->base.remove = esp_pref_remove;
    ns->base.clear = esp_pref_clear;
    ns->base.commit = esp_pref_commit;
    ns->base.iterate = esp_pref_iterate;
    ns->base.iterate_close = esp_pref_iterate_close;
}

static int esp_pref_open(void *user, const char *name_space,
                         h2_pal_pref_open_mode_t mode,
                         h2_pal_pref_namespace_t **out_namespace) {
    h2_esp_pref_namespace_t *ns;
    int rc;
    (void)user;
    if (out_namespace == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_namespace = NULL;
    if (validate_name(name_space, H2_ESP_PREF_NAMESPACE_MAX) != H2_PAL_OK ||
        (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
         mode != H2_PAL_PREF_OPEN_READ_WRITE)) return H2_PAL_ERR_INVALID_ARG;
    rc = pref_lock();
    if (rc != H2_PAL_OK) return rc;
    rc = pref_ensure_ready();
    pref_unlock();
    if (rc != H2_PAL_OK) return rc;
    ns = (h2_esp_pref_namespace_t *)calloc(1u, sizeof(*ns));
    if (ns == NULL) return H2_PAL_ERR_NO_MEMORY;
    namespace_init(ns);
    memcpy(ns->name_space, name_space, strlen(name_space) + 1u);
    ns->mode = mode;
    *out_namespace = &ns->base;
    return H2_PAL_OK;
}

static const h2_pal_pref_vtable_t s_pref_vtable = {.open = esp_pref_open};
static const h2_pal_pref_api_t s_pref_api = {.user = NULL,
                                             .vtable = &s_pref_vtable};

const h2_pal_pref_api_t *h2_esp_platform_pref_api(void) {
    return &s_pref_api;
}
