#include "h2_esp_platform_littlefs_io.h"
#include "h2_esp_platform_pref_nvs_legacy.h"
#include "h2_esp_platform_safe_call.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_ESP_PREF_NVS_NAMESPACE "h2pref"
#define H2_ESP_PREF_MANIFEST_KEY "manifest"
#define H2_ESP_PREF_DATA_KEY_MAX 16u
#define H2_ESP_PREF_MANIFEST_HEADER_SIZE 16u
#define H2_ESP_PREF_MANIFEST_RECORD_SIZE 170u
#define H2_ESP_PREF_MANIFEST_NS_SIZE 65u
#define H2_ESP_PREF_MANIFEST_KEY_SIZE 97u
#define H2_ESP_PREF_NAMESPACE_MAX (H2_ESP_PREF_MANIFEST_NS_SIZE - 1u)
#define H2_ESP_PREF_KEY_MAX (H2_ESP_PREF_MANIFEST_KEY_SIZE - 1u)
#define H2_ESP_PREF_VALUE_MAX (16u * 1024u)
#define H2_ESP_PREF_MANIFEST_KEY_OFFSET H2_ESP_PREF_MANIFEST_NS_SIZE
#define H2_ESP_PREF_MANIFEST_TYPE_OFFSET \
    (H2_ESP_PREF_MANIFEST_NS_SIZE + H2_ESP_PREF_MANIFEST_KEY_SIZE)
#define H2_ESP_PREF_MANIFEST_SLOT_OFFSET \
    (H2_ESP_PREF_MANIFEST_TYPE_OFFSET + 2u)
#define H2_ESP_PREF_MANIFEST_VALUE_SIZE_OFFSET \
    (H2_ESP_PREF_MANIFEST_SLOT_OFFSET + 2u)
#define H2_ESP_PREF_NVS_TASK_STACK_DEPTH 4096u
#define H2_ESP_PREF_NVS_KEY_SIZE 16u

typedef enum h2_esp_pref_nvs_op {
    H2_ESP_PREF_NVS_OP_FLASH_INIT = 1,
    H2_ESP_PREF_NVS_OP_OPEN,
    H2_ESP_PREF_NVS_OP_CLOSE,
    H2_ESP_PREF_NVS_OP_GET_BLOB,
    H2_ESP_PREF_NVS_OP_ERASE_ALL,
    H2_ESP_PREF_NVS_OP_COMMIT,
} h2_esp_pref_nvs_op_t;

typedef struct h2_esp_pref_nvs_call {
    h2_esp_pref_nvs_op_t op;
    esp_err_t result;
    nvs_handle_t handle;
    nvs_open_mode_t open_mode;
    char key[H2_ESP_PREF_NVS_KEY_SIZE];
    void *data;
    size_t data_len;
    int has_output;
} h2_esp_pref_nvs_call_t;

typedef struct h2_esp_pref_manifest_entry {
    char name_space[H2_ESP_PREF_NAMESPACE_MAX + 1u];
    char key[H2_ESP_PREF_KEY_MAX + 1u];
    uint8_t type;
    uint16_t slot;
    uint32_t value_size;
} h2_esp_pref_manifest_entry_t;

typedef struct h2_esp_pref_manifest {
    h2_esp_pref_manifest_entry_t *entries;
    size_t count;
} h2_esp_pref_manifest_t;

static const uint8_t s_manifest_magic[4] = {'H', '2', 'P', 'M'};
static StaticSemaphore_t s_nvs_mutex_storage;
static SemaphoreHandle_t s_nvs_mutex;
static portMUX_TYPE s_nvs_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR pref_nvs_safe_callback(void *context) {
    h2_esp_pref_nvs_call_t *call = (h2_esp_pref_nvs_call_t *)context;
    switch (call->op) {
        case H2_ESP_PREF_NVS_OP_FLASH_INIT:
            call->result = nvs_flash_init();
            break;
        case H2_ESP_PREF_NVS_OP_OPEN:
            call->result = nvs_open(call->key, call->open_mode, &call->handle);
            break;
        case H2_ESP_PREF_NVS_OP_CLOSE:
            nvs_close(call->handle);
            call->result = ESP_OK;
            break;
        case H2_ESP_PREF_NVS_OP_GET_BLOB:
            call->result = nvs_get_blob(
                call->handle, call->key,
                call->has_output ? call->data : NULL, &call->data_len);
            break;
        case H2_ESP_PREF_NVS_OP_ERASE_ALL:
            call->result = nvs_erase_all(call->handle);
            break;
        case H2_ESP_PREF_NVS_OP_COMMIT:
            call->result = nvs_commit(call->handle);
            break;
        default:
            call->result = ESP_ERR_INVALID_ARG;
            break;
    }
}

static SemaphoreHandle_t pref_nvs_mutex(void) {
    portENTER_CRITICAL(&s_nvs_mutex_init_lock);
    if (s_nvs_mutex == NULL) {
        s_nvs_mutex = xSemaphoreCreateMutexStatic(&s_nvs_mutex_storage);
    }
    portEXIT_CRITICAL(&s_nvs_mutex_init_lock);
    return s_nvs_mutex;
}

static esp_err_t pref_nvs_submit(h2_esp_pref_nvs_call_t *call) {
    SemaphoreHandle_t mutex = pref_nvs_mutex();
    h2_pal_result_t rc;
    if (call == NULL || mutex == NULL ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    rc = h2_esp_platform_safe_call(pref_nvs_safe_callback, call,
                                   sizeof(*call),
                                   H2_ESP_PREF_NVS_TASK_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    return rc == H2_PAL_OK ? call->result : ESP_ERR_NO_MEM;
}

static int pref_nvs_copy_key(char out[H2_ESP_PREF_NVS_KEY_SIZE],
                             const char *key) {
    size_t length = 0u;
    if (key == NULL) return 0;
    while (length < H2_ESP_PREF_NVS_KEY_SIZE && key[length] != '\0') {
        ++length;
    }
    if (length == 0u || length >= H2_ESP_PREF_NVS_KEY_SIZE) return 0;
    memcpy(out, key, length + 1u);
    return 1;
}

static esp_err_t pref_nvs_flash_init(void) {
    h2_esp_pref_nvs_call_t call = {.op = H2_ESP_PREF_NVS_OP_FLASH_INIT};
    return pref_nvs_submit(&call);
}

static esp_err_t pref_nvs_open(const char *name, nvs_open_mode_t mode,
                               nvs_handle_t *out_handle) {
    h2_esp_pref_nvs_call_t call = {.op = H2_ESP_PREF_NVS_OP_OPEN};
    esp_err_t result;
    if (out_handle == NULL || !pref_nvs_copy_key(call.key, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    call.open_mode = mode;
    result = pref_nvs_submit(&call);
    if (result == ESP_OK) *out_handle = call.handle;
    return result;
}

static void pref_nvs_close(nvs_handle_t handle) {
    h2_esp_pref_nvs_call_t call = {
        .op = H2_ESP_PREF_NVS_OP_CLOSE,
        .handle = handle,
    };
    (void)pref_nvs_submit(&call);
}

static esp_err_t pref_nvs_get_blob(nvs_handle_t handle, const char *key,
                                   void *out_data, size_t *inout_len) {
    h2_esp_pref_nvs_call_t call = {
        .op = H2_ESP_PREF_NVS_OP_GET_BLOB,
        .handle = handle,
    };
    uint8_t *scratch = NULL;
    size_t scratch_capacity = 0u;
    size_t capacity;
    esp_err_t result;
    if (inout_len == NULL || !pref_nvs_copy_key(call.key, key)) {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = *inout_len;
    call.data_len = capacity;
    call.has_output = out_data != NULL;
    if (out_data != NULL) {
        h2_pal_result_t rc = h2_esp_platform_safe_io_acquire(
            &scratch, &scratch_capacity);
        if (rc != H2_PAL_OK || scratch == NULL) {
            if (rc == H2_PAL_OK) h2_esp_platform_safe_io_release();
            return ESP_ERR_NO_MEM;
        }
        call.data = scratch;
        if (call.data_len > scratch_capacity) {
            call.data_len = scratch_capacity;
        }
    }
    result = pref_nvs_submit(&call);
    *inout_len = call.data_len;
    if (result == ESP_OK && out_data != NULL) {
        if (call.data_len > capacity) result = ESP_ERR_INVALID_SIZE;
        else if (call.data_len > 0u) memcpy(out_data, scratch, call.data_len);
    }
    if (out_data != NULL) h2_esp_platform_safe_io_release();
    return result;
}

static esp_err_t pref_nvs_erase_all(nvs_handle_t handle) {
    h2_esp_pref_nvs_call_t call = {
        .op = H2_ESP_PREF_NVS_OP_ERASE_ALL,
        .handle = handle,
    };
    return pref_nvs_submit(&call);
}

static esp_err_t pref_nvs_commit(nvs_handle_t handle) {
    h2_esp_pref_nvs_call_t call = {
        .op = H2_ESP_PREF_NVS_OP_COMMIT,
        .handle = handle,
    };
    return pref_nvs_submit(&call);
}

static int nvs_result(esp_err_t error) {
    switch (error) {
        case ESP_OK:
            return H2_PAL_OK;
        case ESP_ERR_NVS_NOT_FOUND:
            return H2_PAL_ERR_NOT_FOUND;
        case ESP_ERR_NO_MEM:
            return H2_PAL_ERR_NO_MEMORY;
        case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        case ESP_ERR_NVS_NO_FREE_PAGES:
            return H2_PAL_ERR_NO_SPACE;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_NVS_INVALID_NAME:
        case ESP_ERR_NVS_KEY_TOO_LONG:
            return H2_PAL_ERR_INVALID_ARG;
        default:
            return H2_PAL_ERR_IO;
    }
}

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void make_data_key(char out[H2_ESP_PREF_DATA_KEY_MAX], uint16_t slot) {
    snprintf(out, H2_ESP_PREF_DATA_KEY_MAX, "d%04x", (unsigned)slot);
}

static void manifest_free(h2_esp_pref_manifest_t *manifest) {
    if (manifest == NULL) return;
    free(manifest->entries);
    manifest->entries = NULL;
    manifest->count = 0u;
}

static int copy_manifest_text(char *out, size_t out_size,
                              const uint8_t *data, size_t data_size) {
    const void *end = memchr(data, '\0', data_size);
    size_t length;
    if (end == NULL) return H2_PAL_ERR_IO;
    length = (size_t)((const uint8_t *)end - data);
    if (length == 0u || length >= out_size) return H2_PAL_ERR_IO;
    memcpy(out, data, length + 1u);
    return H2_PAL_OK;
}

static int manifest_decode_entry(h2_esp_pref_manifest_entry_t *entry,
                                 const uint8_t *record) {
    int rc = copy_manifest_text(entry->name_space, sizeof(entry->name_space),
                                record, H2_ESP_PREF_MANIFEST_NS_SIZE);
    if (rc != H2_PAL_OK) return rc;
    rc = copy_manifest_text(entry->key, sizeof(entry->key),
                            record + H2_ESP_PREF_MANIFEST_KEY_OFFSET,
                            H2_ESP_PREF_MANIFEST_KEY_SIZE);
    if (rc != H2_PAL_OK) return rc;
    entry->type = record[H2_ESP_PREF_MANIFEST_TYPE_OFFSET];
    entry->slot = read_le16(record + H2_ESP_PREF_MANIFEST_SLOT_OFFSET);
    entry->value_size =
        read_le32(record + H2_ESP_PREF_MANIFEST_VALUE_SIZE_OFFSET);
    if (entry->type < (uint8_t)H2_PAL_PREF_ENTRY_BLOB ||
        entry->type > (uint8_t)H2_PAL_PREF_ENTRY_BOOL || entry->slot == 0u ||
        entry->value_size > H2_ESP_PREF_VALUE_MAX) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static int manifest_load(nvs_handle_t handle,
                         h2_esp_pref_manifest_t *manifest) {
    size_t length = 0u;
    size_t expected_length;
    uint32_t count;
    uint8_t *data;
    size_t index;
    esp_err_t error;
    if (manifest == NULL) return H2_PAL_ERR_INVALID_ARG;
    manifest->entries = NULL;
    manifest->count = 0u;
    error = pref_nvs_get_blob(handle, H2_ESP_PREF_MANIFEST_KEY, NULL,
                              &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) return H2_PAL_OK;
    if (error != ESP_OK) return nvs_result(error);
    if (length < H2_ESP_PREF_MANIFEST_HEADER_SIZE) return H2_PAL_ERR_IO;
    data = (uint8_t *)malloc(length);
    if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
    error = pref_nvs_get_blob(handle, H2_ESP_PREF_MANIFEST_KEY, data,
                              &length);
    if (error != ESP_OK) {
        free(data);
        return nvs_result(error);
    }
    if (memcmp(data, s_manifest_magic, sizeof(s_manifest_magic)) != 0 ||
        data[4] != 1u ||
        read_le32(data + 12u) != H2_ESP_PREF_MANIFEST_RECORD_SIZE) {
        free(data);
        return H2_PAL_ERR_IO;
    }
    count = read_le32(data + 8u);
    if ((size_t)count >
        (SIZE_MAX - H2_ESP_PREF_MANIFEST_HEADER_SIZE) /
            H2_ESP_PREF_MANIFEST_RECORD_SIZE) {
        free(data);
        return H2_PAL_ERR_IO;
    }
    expected_length = H2_ESP_PREF_MANIFEST_HEADER_SIZE +
                      (size_t)count * H2_ESP_PREF_MANIFEST_RECORD_SIZE;
    if (length != expected_length) {
        free(data);
        return H2_PAL_ERR_IO;
    }
    if (count > 0u) {
        manifest->entries = (h2_esp_pref_manifest_entry_t *)calloc(
            (size_t)count, sizeof(*manifest->entries));
        if (manifest->entries == NULL) {
            free(data);
            return H2_PAL_ERR_NO_MEMORY;
        }
        manifest->count = (size_t)count;
        for (index = 0u; index < manifest->count; ++index) {
            int rc = manifest_decode_entry(
                &manifest->entries[index],
                data + H2_ESP_PREF_MANIFEST_HEADER_SIZE +
                    index * H2_ESP_PREF_MANIFEST_RECORD_SIZE);
            if (rc != H2_PAL_OK) {
                free(data);
                manifest_free(manifest);
                return rc;
            }
        }
    }
    free(data);
    return H2_PAL_OK;
}

static int read_slot_copy(nvs_handle_t handle, const char *value_key,
                          uint8_t **out_data, size_t *out_length) {
    uint8_t *data;
    size_t length = 0u;
    esp_err_t error = pref_nvs_get_blob(handle, value_key, NULL, &length);
    if (error != ESP_OK) return nvs_result(error);
    data = (uint8_t *)malloc(length == 0u ? 1u : length);
    if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
    error = pref_nvs_get_blob(handle, value_key, data, &length);
    if (error != ESP_OK) {
        free(data);
        return nvs_result(error);
    }
    *out_data = data;
    *out_length = length;
    return H2_PAL_OK;
}

int h2_esp_pref_legacy_copy(h2_esp_pref_store_t *store) {
    nvs_handle_t handle;
    h2_esp_pref_manifest_t manifest;
    size_t index;
    esp_err_t error;
    int rc = H2_PAL_OK;
    if (store == NULL) return H2_PAL_ERR_INVALID_ARG;
    error = pref_nvs_flash_init();
    if (error != ESP_OK) return nvs_result(error);
    error = pref_nvs_open(H2_ESP_PREF_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return H2_PAL_OK;
    if (error != ESP_OK) return nvs_result(error);
    rc = manifest_load(handle, &manifest);
    if (rc != H2_PAL_OK) {
        pref_nvs_close(handle);
        return rc;
    }
    for (index = 0u; index < manifest.count; ++index) {
        const h2_esp_pref_manifest_entry_t *entry = &manifest.entries[index];
        char value_key[H2_ESP_PREF_DATA_KEY_MAX];
        uint8_t *value = NULL;
        size_t value_size = 0u;
        uint8_t bool_value;
        const uint8_t *expected_value;
        size_t expected_size;
        h2_pal_pref_entry_type_t expected_type =
            (h2_pal_pref_entry_type_t)entry->type;
        make_data_key(value_key, entry->slot);
        rc = read_slot_copy(handle, value_key, &value, &value_size);
        if (rc != H2_PAL_OK) break;
        if (entry->value_size == 0u && value_size == 1u) value_size = 0u;
        if (value_size != entry->value_size) {
            free(value);
            rc = H2_PAL_ERR_IO;
            break;
        }
        if (expected_type == H2_PAL_PREF_ENTRY_BOOL) {
            int32_t old_bool;
            if (value_size != sizeof(old_bool)) {
                free(value);
                rc = H2_PAL_ERR_IO;
                break;
            }
            memcpy(&old_bool, value, sizeof(old_bool));
            bool_value = old_bool != 0 ? 1u : 0u;
            expected_value = &bool_value;
            expected_size = sizeof(bool_value);
            rc = h2_esp_pref_io_set(store, entry->name_space, entry->key,
                                    H2_PAL_PREF_ENTRY_BOOL, &bool_value,
                                    sizeof(bool_value));
        } else {
            expected_value = value;
            expected_size = value_size;
            rc = h2_esp_pref_io_set(store, entry->name_space, entry->key,
                                    expected_type, value, value_size);
        }
        if (rc == H2_PAL_OK) {
            uint8_t *copied = NULL;
            size_t copied_size = 0u;
            rc = h2_esp_pref_io_get(store, entry->name_space, entry->key,
                                    expected_type, &copied, &copied_size);
            if (rc == H2_PAL_OK &&
                (copied_size != expected_size ||
                 (copied_size > 0u &&
                  memcmp(copied, expected_value, copied_size) != 0))) {
                rc = H2_PAL_ERR_IO;
            }
            free(copied);
        }
        free(value);
        if (rc != H2_PAL_OK) break;
    }
    manifest_free(&manifest);
    pref_nvs_close(handle);
    return rc;
}

int h2_esp_pref_legacy_cleanup(void) {
    nvs_handle_t handle;
    esp_err_t error = pref_nvs_flash_init();
    if (error != ESP_OK) return nvs_result(error);
    error = pref_nvs_open(H2_ESP_PREF_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return H2_PAL_OK;
    if (error != ESP_OK) return nvs_result(error);
    pref_nvs_close(handle);
    error = pref_nvs_open(H2_ESP_PREF_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return nvs_result(error);
    error = pref_nvs_erase_all(handle);
    if (error == ESP_OK) error = pref_nvs_commit(handle);
    if (error == ESP_OK) {
        size_t remaining = 0u;
        error = pref_nvs_get_blob(handle, H2_ESP_PREF_MANIFEST_KEY, NULL,
                                  &remaining);
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        else if (error == ESP_OK) error = ESP_FAIL;
    }
    pref_nvs_close(handle);
    return nvs_result(error);
}
