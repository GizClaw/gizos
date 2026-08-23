#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define H2_ESP_DISK_PARTITION_LOADER 1u
#define H2_ESP_DISK_PARTITION_APP 2u
#define H2_ESP_DISK_PARTITION_COREDUMP 3u
#define H2_ESP_DISK_CHUNK_SIZE (16u * 1024u)
#define H2_ESP_DISK_SAFE_STACK_DEPTH 4096u

typedef enum h2_esp_disk_op {
    H2_ESP_DISK_OP_READ = 1,
    H2_ESP_DISK_OP_WRITE,
    H2_ESP_DISK_OP_ERASE,
} h2_esp_disk_op_t;

typedef struct h2_esp_disk_call {
    h2_esp_disk_op_t op;
    const esp_partition_t *partition;
    size_t offset;
    void *data;
    size_t len;
    esp_err_t result;
} h2_esp_disk_call_t;

static StaticSemaphore_t s_disk_mutex_storage;
static SemaphoreHandle_t s_disk_mutex;
static portMUX_TYPE s_disk_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t disk_mutex(void) {
    portENTER_CRITICAL(&s_disk_mutex_init_lock);
    if (s_disk_mutex == NULL) {
        s_disk_mutex = xSemaphoreCreateMutexStatic(&s_disk_mutex_storage);
    }
    portEXIT_CRITICAL(&s_disk_mutex_init_lock);
    return s_disk_mutex;
}

static void IRAM_ATTR disk_safe_callback(void *context) {
    h2_esp_disk_call_t *call = (h2_esp_disk_call_t *)context;
    if (call->op == H2_ESP_DISK_OP_READ) {
        call->result = esp_partition_read(
            call->partition, call->offset, call->data, call->len);
    } else if (call->op == H2_ESP_DISK_OP_WRITE) {
        call->result = esp_partition_write(
            call->partition, call->offset, call->data, call->len);
    } else if (call->op == H2_ESP_DISK_OP_ERASE) {
        call->result = esp_partition_erase_range(
            call->partition, call->offset, call->len);
    } else {
        call->result = ESP_ERR_INVALID_ARG;
    }
}

static h2_pal_result_t disk_safe_call(h2_esp_disk_call_t *call) {
    SemaphoreHandle_t mutex = disk_mutex();
    h2_pal_result_t rc;
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TASK;
    }
    rc = h2_esp_platform_safe_call(
        disk_safe_callback, call, sizeof(*call), H2_ESP_DISK_SAFE_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return call->result == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static const esp_partition_t *disk_partition_for_id(uint32_t id) {
    if (id == H2_ESP_DISK_PARTITION_LOADER) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    if (id == H2_ESP_DISK_PARTITION_APP) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    }
    if (id == H2_ESP_DISK_PARTITION_COREDUMP) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    }
    return NULL;
}

static h2_pal_result_t fill_disk_partition(uint32_t id, h2_pal_disk_partition_t *out_partition) {
    const esp_partition_t *partition = disk_partition_for_id(id);
    size_t len;

    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    out_partition->id = id;
    out_partition->size = partition->size;
    out_partition->erase_block_size = 4096u;
    out_partition->write_alignment = 1u;
    out_partition->flags = H2_PAL_DISK_PARTITION_FLAG_READABLE;
    if (id == H2_ESP_DISK_PARTITION_APP || id == H2_ESP_DISK_PARTITION_COREDUMP) {
        out_partition->flags |= H2_PAL_DISK_PARTITION_FLAG_WRITABLE | H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
    }
    if (partition->type == ESP_PARTITION_TYPE_APP) {
        out_partition->flags |= H2_PAL_DISK_PARTITION_FLAG_BOOTABLE;
    }
    len = strnlen(partition->label, sizeof(partition->label));
    if (len >= sizeof(out_partition->name)) {
        len = sizeof(out_partition->name) - 1u;
    }
    memcpy(out_partition->name, partition->label, len);
    out_partition->name[len] = '\0';
    return H2_PAL_OK;
}

static h2_pal_result_t disk_list_partitions(
    void *user,
    h2_pal_disk_partition_cb_t cb,
    void *cb_user) {
    static const uint32_t ids[] = {
        H2_ESP_DISK_PARTITION_LOADER,
        H2_ESP_DISK_PARTITION_APP,
        H2_ESP_DISK_PARTITION_COREDUMP,
    };

    (void)user;
    if (cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        h2_pal_disk_partition_t partition;
        h2_pal_result_t rc = fill_disk_partition(ids[i], &partition);
        if (rc == H2_PAL_ERR_NOT_FOUND) {
            continue;
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = cb(cb_user, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t disk_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    (void)user;
    return fill_disk_partition(partition_id, out_partition);
}

static h2_pal_result_t disk_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    const esp_partition_t *partition;

    (void)user;
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = disk_partition_for_id(partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (offset > partition->size || len > partition->size - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    uint8_t *internal_data = NULL;
    size_t internal_capacity = 0u;
    h2_pal_result_t rc = h2_esp_platform_safe_io_acquire(
        &internal_data, &internal_capacity);
    if (rc != H2_PAL_OK || internal_data == NULL ||
        internal_capacity < H2_ESP_DISK_CHUNK_SIZE) {
        if (rc == H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
        }
        return rc == H2_PAL_OK ? H2_PAL_ERR_NO_MEMORY : rc;
    }
    for (size_t done = 0u; done < len;) {
        size_t chunk_len = len - done;
        if (chunk_len > internal_capacity) {
            chunk_len = internal_capacity;
        }
        h2_esp_disk_call_t call = {
            .op = H2_ESP_DISK_OP_READ,
            .partition = partition,
            .offset = (size_t)offset + done,
            .data = internal_data,
            .len = chunk_len,
        };
        rc = disk_safe_call(&call);
        if (rc != H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
            return rc;
        }
        memcpy((uint8_t *)data + done, internal_data, chunk_len);
        done += chunk_len;
    }
    h2_esp_platform_safe_io_release();
    return H2_PAL_OK;
}

static h2_pal_result_t disk_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    const esp_partition_t *partition;

    (void)user;
    partition = disk_partition_for_id(partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (offset > partition->size || len > partition->size - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_esp_disk_call_t call = {
        .op = H2_ESP_DISK_OP_ERASE,
        .partition = partition,
        .offset = (size_t)offset,
        .len = (size_t)len,
    };
    return disk_safe_call(&call);
}

static h2_pal_result_t disk_write(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    const void *data,
    size_t len) {
    const esp_partition_t *partition;

    (void)user;
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = disk_partition_for_id(partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (offset > partition->size || len > partition->size - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    uint8_t *internal_data = NULL;
    size_t internal_capacity = 0u;
    h2_pal_result_t rc = h2_esp_platform_safe_io_acquire(
        &internal_data, &internal_capacity);
    if (rc != H2_PAL_OK || internal_data == NULL ||
        internal_capacity < H2_ESP_DISK_CHUNK_SIZE) {
        if (rc == H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
        }
        return rc == H2_PAL_OK ? H2_PAL_ERR_NO_MEMORY : rc;
    }
    for (size_t done = 0u; done < len;) {
        size_t chunk_len = len - done;
        if (chunk_len > internal_capacity) {
            chunk_len = internal_capacity;
        }
        memcpy(internal_data, (const uint8_t *)data + done, chunk_len);
        h2_esp_disk_call_t call = {
            .op = H2_ESP_DISK_OP_WRITE,
            .partition = partition,
            .offset = (size_t)offset + done,
            .data = internal_data,
            .len = chunk_len,
        };
        rc = disk_safe_call(&call);
        if (rc != H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
            return rc;
        }
        done += chunk_len;
    }
    h2_esp_platform_safe_io_release();
    return H2_PAL_OK;
}

static h2_pal_result_t disk_flush(void *user, uint32_t partition_id) {
    (void)user;
    (void)partition_id;
    return H2_PAL_OK;
}

const h2_pal_disk_api_t *h2_esp_platform_disk_api(void) {
    static const h2_pal_disk_vtable_t vtable = {
        .list_partitions = disk_list_partitions,
        .get_partition = disk_get_partition,
        .read = disk_read,
        .erase = disk_erase,
        .write = disk_write,
        .flush = disk_flush,
    };
    static const h2_pal_disk_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
