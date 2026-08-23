#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define H2_ESP_SPIFFS_PATH_SIZE 192u
#define H2_ESP_SPIFFS_CHUNK_SIZE 4096u
#define H2_ESP_SPIFFS_SAFE_STACK_DEPTH 4096u

static StaticSemaphore_t s_spiffs_safe_mutex_storage;
static SemaphoreHandle_t s_spiffs_safe_mutex;
static portMUX_TYPE s_spiffs_safe_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct h2_esp_platform_spiffs_context h2_esp_platform_spiffs_context_t;

typedef enum h2_esp_spiffs_safe_op {
    H2_ESP_SPIFFS_OPEN = 1,
    H2_ESP_SPIFFS_READ,
    H2_ESP_SPIFFS_WRITE,
    H2_ESP_SPIFFS_SYNC,
    H2_ESP_SPIFFS_CLOSE,
    H2_ESP_SPIFFS_STAT,
    H2_ESP_SPIFFS_REMOVE,
    H2_ESP_SPIFFS_RENAME,
    H2_ESP_SPIFFS_MOUNT,
    H2_ESP_SPIFFS_UNMOUNT,
} h2_esp_spiffs_safe_op_t;

typedef struct h2_esp_spiffs_safe_call {
    h2_esp_spiffs_safe_op_t op;
    h2_esp_platform_spiffs_context_t *context;
    h2_pal_fs_file_t *file;
    h2_pal_fs_open_mode_t mode;
    char path[H2_ESP_SPIFFS_PATH_SIZE];
    char second_path[H2_ESP_SPIFFS_PATH_SIZE];
    char base_path[H2_ESP_SPIFFS_PATH_SIZE];
    uint8_t *data;
    size_t len;
    size_t processed;
    h2_pal_fs_stat_t stat_value;
    uint32_t max_files;
    int format_if_mount_failed;
    int result;
} h2_esp_spiffs_safe_call_t;

struct h2_esp_platform_spiffs_context {
    const h2_esp_platform_spiffs_config_t *config;
};

typedef struct h2_esp_platform_file {
    FILE *fp;
} h2_esp_platform_file_t;

static h2_esp_platform_spiffs_config_t s_spiffs_config;
static h2_esp_platform_spiffs_context_t s_spiffs_context = {
    .config = &s_spiffs_config,
};

static int map_errno(int fallback) {
    switch (errno) {
    case 0:
        return fallback;
    case ENOENT:
        return H2_PAL_FS_ERR_NOT_FOUND;
    case ENOMEM:
        return H2_PAL_FS_ERR_NO_MEMORY;
    case ENOSPC:
        return H2_PAL_FS_ERR_NO_SPACE;
    case EINVAL:
        return H2_PAL_FS_ERR_INVALID_ARG;
    default:
        return H2_PAL_FS_ERR_IO;
    }
}

static int translate_path_exact(const h2_esp_platform_spiffs_context_t *ctx, const char *path, char *out, size_t out_len) {
    int len;

    if (ctx == NULL || ctx->config == NULL || ctx->config->base_path == NULL ||
        path == NULL || out == NULL || out_len == 0u || path[0] != '/') {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (strstr(path, "/../") != NULL || strstr(path, "/./") != NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    len = snprintf(out, out_len, "%s%s", ctx->config->base_path, path);
    if (len < 0 || (size_t)len >= out_len) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    return H2_PAL_FS_OK;
}

static int translate_path_flattened(const h2_esp_platform_spiffs_context_t *ctx, const char *path, char *out, size_t out_len) {
    int rc = translate_path_exact(ctx, path, out, out_len);
    size_t base_len;

    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    base_len = strlen(ctx->config->base_path);
    for (size_t i = base_len + 1u; out[i] != '\0'; ++i) {
        if (out[i] == '/') {
            out[i] = '_';
        }
    }
    return H2_PAL_FS_OK;
}

static int spiffs_mkdir(void *user, const char *path) {
    (void)user;
    (void)path;
    return H2_PAL_FS_OK;
}

static int spiffs_open_unsafe(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file) {
    h2_esp_platform_spiffs_context_t *ctx = (h2_esp_platform_spiffs_context_t *)user;
    char real_path[192];
    char fallback_path[192];
    h2_esp_platform_file_t *file;
    const char *fmode;
    int rc;

    if (out_file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_file = NULL;

    if (mode == H2_PAL_FS_OPEN_READ) {
        fmode = "rb";
    } else if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
        fmode = "wb";
    } else {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }

    file = (h2_esp_platform_file_t *)calloc(1u, sizeof(*file));
    if (file == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    rc = translate_path_exact(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        free(file);
        return rc;
    }
    rc = translate_path_flattened(ctx, path, fallback_path, sizeof(fallback_path));
    if (rc != H2_PAL_FS_OK) {
        free(file);
        return rc;
    }
    errno = 0;
    file->fp = fopen(real_path, fmode);
    if (file->fp == NULL && strcmp(real_path, fallback_path) != 0) {
        errno = 0;
        file->fp = fopen(fallback_path, fmode);
    }
    if (file->fp == NULL) {
        free(file);
        return map_errno(H2_PAL_FS_ERR_IO);
    }
    *out_file = (h2_pal_fs_file_t *)file;
    return H2_PAL_FS_OK;
}

static int spiffs_read_unsafe(void *user, h2_pal_fs_file_t *raw_file, void *data, size_t len, size_t *out_read) {
    h2_esp_platform_file_t *file = (h2_esp_platform_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL || data == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    *out_read = fread(data, 1u, len, file->fp);
    if (ferror(file->fp) != 0) {
        return map_errno(H2_PAL_FS_ERR_IO);
    }
    return H2_PAL_FS_OK;
}

static int spiffs_write_unsafe(void *user, h2_pal_fs_file_t *raw_file, const void *data, size_t len, size_t *out_written) {
    h2_esp_platform_file_t *file = (h2_esp_platform_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL || (data == NULL && len != 0u) || out_written == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    *out_written = fwrite(data, 1u, len, file->fp);
    if (*out_written == 0u && len != 0u) {
        return map_errno(H2_PAL_FS_ERR_IO);
    }
    if (*out_written < len && ferror(file->fp) != 0) {
        return map_errno(H2_PAL_FS_ERR_IO);
    }
    return H2_PAL_FS_OK;
}

static int spiffs_sync_unsafe(void *user, h2_pal_fs_file_t *raw_file) {
    h2_esp_platform_file_t *file = (h2_esp_platform_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    return fflush(file->fp) == 0 ? H2_PAL_FS_OK : map_errno(H2_PAL_FS_ERR_IO);
}

static int spiffs_close_unsafe(void *user, h2_pal_fs_file_t *raw_file) {
    h2_esp_platform_file_t *file = (h2_esp_platform_file_t *)raw_file;
    int rc = H2_PAL_FS_OK;
    (void)user;

    if (file == NULL || file->fp == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    if (fclose(file->fp) != 0) {
        rc = map_errno(H2_PAL_FS_ERR_IO);
    }
    free(file);
    return rc;
}

static int spiffs_stat_unsafe(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
    h2_esp_platform_spiffs_context_t *ctx = (h2_esp_platform_spiffs_context_t *)user;
    char real_path[192];
    char fallback_path[192];
    struct stat st;
    int rc;

    if (out_stat == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    rc = translate_path_exact(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    errno = 0;
    if (stat(real_path, &st) != 0) {
        rc = translate_path_flattened(ctx, path, fallback_path, sizeof(fallback_path));
        if (rc != H2_PAL_FS_OK) {
            return rc;
        }
        if (strcmp(real_path, fallback_path) == 0) {
            return map_errno(H2_PAL_FS_ERR_IO);
        }
        errno = 0;
        if (stat(fallback_path, &st) != 0) {
            return map_errno(H2_PAL_FS_ERR_IO);
        }
    }
    out_stat->size = (uint64_t)st.st_size;
    out_stat->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return H2_PAL_FS_OK;
}

static int spiffs_remove_unsafe(void *user, const char *path) {
    h2_esp_platform_spiffs_context_t *ctx = (h2_esp_platform_spiffs_context_t *)user;
    char real_path[192];
    char fallback_path[192];
    int rc = translate_path_exact(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    errno = 0;
    if (remove(real_path) == 0) {
        return H2_PAL_FS_OK;
    }
    rc = translate_path_flattened(ctx, path, fallback_path, sizeof(fallback_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (strcmp(real_path, fallback_path) == 0) {
        return map_errno(H2_PAL_FS_ERR_IO);
    }
    errno = 0;
    return remove(fallback_path) == 0 ? H2_PAL_FS_OK : map_errno(H2_PAL_FS_ERR_IO);
}

static int spiffs_rename_unsafe(void *user, const char *old_path, const char *new_path) {
    h2_esp_platform_spiffs_context_t *ctx = (h2_esp_platform_spiffs_context_t *)user;
    char real_old_path[192];
    char real_new_path[192];
    int rc = translate_path_flattened(ctx, old_path, real_old_path, sizeof(real_old_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = translate_path_flattened(ctx, new_path, real_new_path, sizeof(real_new_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    errno = 0;
    return rename(real_old_path, real_new_path) == 0 ? H2_PAL_FS_OK : map_errno(H2_PAL_FS_ERR_IO);
}

static void IRAM_ATTR spiffs_safe_callback(void *context) {
    h2_esp_spiffs_safe_call_t *call = (h2_esp_spiffs_safe_call_t *)context;
    h2_esp_platform_spiffs_config_t local_config = {
        .base_path = call->base_path,
    };
    h2_esp_platform_spiffs_context_t local_context = {
        .config = &local_config,
    };
    if (call->op == H2_ESP_SPIFFS_OPEN) {
        call->result = spiffs_open_unsafe(
            &local_context, call->path, call->mode, &call->file);
    } else if (call->op == H2_ESP_SPIFFS_READ) {
        call->result = spiffs_read_unsafe(
            &local_context, call->file, call->data, call->len, &call->processed);
    } else if (call->op == H2_ESP_SPIFFS_WRITE) {
        call->result = spiffs_write_unsafe(
            &local_context, call->file, call->data, call->len, &call->processed);
    } else if (call->op == H2_ESP_SPIFFS_SYNC) {
        call->result = spiffs_sync_unsafe(&local_context, call->file);
    } else if (call->op == H2_ESP_SPIFFS_CLOSE) {
        call->result = spiffs_close_unsafe(&local_context, call->file);
    } else if (call->op == H2_ESP_SPIFFS_STAT) {
        call->result = spiffs_stat_unsafe(
            &local_context, call->path, &call->stat_value);
    } else if (call->op == H2_ESP_SPIFFS_REMOVE) {
        call->result = spiffs_remove_unsafe(&local_context, call->path);
    } else if (call->op == H2_ESP_SPIFFS_RENAME) {
        call->result = spiffs_rename_unsafe(
            &local_context, call->path, call->second_path);
    } else if (call->op == H2_ESP_SPIFFS_MOUNT) {
        esp_vfs_spiffs_conf_t config = {
            .base_path = call->path,
            .partition_label = call->second_path,
            .max_files = call->max_files != 0u ? call->max_files : 16u,
            .format_if_mount_failed = call->format_if_mount_failed != 0,
        };
        esp_err_t err = esp_vfs_spiffs_register(&config);
        call->result = err == ESP_OK || err == ESP_ERR_INVALID_STATE
            ? H2_PAL_FS_OK
            : H2_PAL_FS_ERR_IO;
    } else if (call->op == H2_ESP_SPIFFS_UNMOUNT) {
        esp_err_t err = esp_vfs_spiffs_unregister(call->path);
        call->result = err == ESP_OK || err == ESP_ERR_INVALID_STATE
            ? H2_PAL_FS_OK
            : H2_PAL_FS_ERR_IO;
    } else {
        call->result = H2_PAL_FS_ERR_INVALID_ARG;
    }
}

static SemaphoreHandle_t spiffs_safe_mutex(void) {
    portENTER_CRITICAL(&s_spiffs_safe_mutex_init_lock);
    if (s_spiffs_safe_mutex == NULL) {
        s_spiffs_safe_mutex =
            xSemaphoreCreateMutexStatic(&s_spiffs_safe_mutex_storage);
    }
    portEXIT_CRITICAL(&s_spiffs_safe_mutex_init_lock);
    return s_spiffs_safe_mutex;
}

static int spiffs_run_safe(h2_esp_spiffs_safe_call_t *call) {
    if (call->context != NULL && call->context->config != NULL &&
        call->context->config->base_path != NULL) {
        int written = snprintf(
            call->base_path,
            sizeof(call->base_path),
            "%s",
            call->context->config->base_path);
        if (written < 0 || (size_t)written >= sizeof(call->base_path)) {
            return H2_PAL_FS_ERR_NO_SPACE;
        }
    }
    SemaphoreHandle_t mutex = spiffs_safe_mutex();
    if (mutex == NULL ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_FS_ERR_IO;
    }
    h2_pal_result_t rc = h2_esp_platform_safe_call(
        spiffs_safe_callback,
        call,
        sizeof(*call),
        H2_ESP_SPIFFS_SAFE_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    return rc == H2_PAL_OK ? call->result : rc;
}

static int spiffs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    if (path == NULL || out_file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    h2_esp_spiffs_safe_call_t call = {
        .op = H2_ESP_SPIFFS_OPEN,
        .context = (h2_esp_platform_spiffs_context_t *)user,
        .mode = mode,
    };
    int written = snprintf(call.path, sizeof(call.path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(call.path)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    int rc = spiffs_run_safe(&call);
    if (rc == H2_PAL_FS_OK) {
        *out_file = call.file;
    }
    return rc;
}

static int spiffs_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    if (file == NULL || data == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    if (len == 0u) {
        return H2_PAL_FS_OK;
    }
    uint8_t *internal_data = heap_caps_malloc(
        H2_ESP_SPIFFS_CHUNK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_data == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int rc = H2_PAL_FS_OK;
    while (*out_read < len) {
        h2_esp_spiffs_safe_call_t call = {
            .op = H2_ESP_SPIFFS_READ,
            .context = (h2_esp_platform_spiffs_context_t *)user,
            .file = file,
            .data = internal_data,
            .len = len - *out_read > H2_ESP_SPIFFS_CHUNK_SIZE
                ? H2_ESP_SPIFFS_CHUNK_SIZE
                : len - *out_read,
        };
        rc = spiffs_run_safe(&call);
        if (rc != H2_PAL_FS_OK) {
            break;
        }
        memcpy((uint8_t *)data + *out_read, internal_data, call.processed);
        *out_read += call.processed;
        if (call.processed < call.len) {
            break;
        }
    }
    heap_caps_free(internal_data);
    return rc;
}

static int spiffs_write(
    void *user,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    if (file == NULL || (data == NULL && len != 0u) || out_written == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    if (len == 0u) {
        return H2_PAL_FS_OK;
    }
    uint8_t *internal_data = heap_caps_malloc(
        H2_ESP_SPIFFS_CHUNK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_data == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int rc = H2_PAL_FS_OK;
    while (*out_written < len) {
        h2_esp_spiffs_safe_call_t call = {
            .op = H2_ESP_SPIFFS_WRITE,
            .context = (h2_esp_platform_spiffs_context_t *)user,
            .file = file,
            .data = internal_data,
            .len = len - *out_written > H2_ESP_SPIFFS_CHUNK_SIZE
                ? H2_ESP_SPIFFS_CHUNK_SIZE
                : len - *out_written,
        };
        memcpy(internal_data, (const uint8_t *)data + *out_written, call.len);
        rc = spiffs_run_safe(&call);
        *out_written += call.processed;
        if (rc != H2_PAL_FS_OK) {
            break;
        }
    }
    heap_caps_free(internal_data);
    return rc;
}

static int spiffs_file_call(
    h2_esp_spiffs_safe_op_t op,
    void *user,
    h2_pal_fs_file_t *file) {
    h2_esp_spiffs_safe_call_t call = {
        .op = op,
        .context = (h2_esp_platform_spiffs_context_t *)user,
        .file = file,
    };
    return spiffs_run_safe(&call);
}

static int spiffs_sync(void *user, h2_pal_fs_file_t *file) {
    return file != NULL
        ? spiffs_file_call(H2_ESP_SPIFFS_SYNC, user, file)
        : H2_PAL_FS_ERR_INVALID_ARG;
}

static int spiffs_close(void *user, h2_pal_fs_file_t *file) {
    return file != NULL
        ? spiffs_file_call(H2_ESP_SPIFFS_CLOSE, user, file)
        : H2_PAL_FS_ERR_INVALID_ARG;
}

static int spiffs_path_call(
    h2_esp_spiffs_safe_op_t op,
    void *user,
    const char *path,
    const char *second_path,
    h2_pal_fs_stat_t *out_stat) {
    if (path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_spiffs_safe_call_t call = {
        .op = op,
        .context = (h2_esp_platform_spiffs_context_t *)user,
    };
    int written = snprintf(call.path, sizeof(call.path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(call.path)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    if (second_path != NULL) {
        written = snprintf(
            call.second_path, sizeof(call.second_path), "%s", second_path);
        if (written < 0 || (size_t)written >= sizeof(call.second_path)) {
            return H2_PAL_FS_ERR_NO_SPACE;
        }
    }
    int rc = spiffs_run_safe(&call);
    if (rc == H2_PAL_FS_OK && out_stat != NULL) {
        *out_stat = call.stat_value;
    }
    return rc;
}

static int spiffs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
    return out_stat != NULL
        ? spiffs_path_call(H2_ESP_SPIFFS_STAT, user, path, NULL, out_stat)
        : H2_PAL_FS_ERR_INVALID_ARG;
}

static int spiffs_remove(void *user, const char *path) {
    return spiffs_path_call(H2_ESP_SPIFFS_REMOVE, user, path, NULL, NULL);
}

static int spiffs_rename(void *user, const char *old_path, const char *new_path) {
    return new_path != NULL
        ? spiffs_path_call(
              H2_ESP_SPIFFS_RENAME, user, old_path, new_path, NULL)
        : H2_PAL_FS_ERR_INVALID_ARG;
}

int h2_esp_platform_spiffs_fs_init(h2_pal_fs_api_t *fs, const h2_esp_platform_spiffs_config_t *config) {
    static const h2_pal_fs_vtable_t osal_vtable = {
        .mkdir = spiffs_mkdir,
        .open = spiffs_open,
        .read = spiffs_read,
        .write = spiffs_write,
        .sync = spiffs_sync,
        .close = spiffs_close,
        .stat = spiffs_stat,
        .remove = spiffs_remove,
        .rename = spiffs_rename,
    };

static const h2_pal_fs_api_t osal = {
    .user = &s_spiffs_context,
    .vtable = &osal_vtable,
};
    if (fs == NULL || config == NULL || config->base_path == NULL || config->partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    s_spiffs_config = *config;

    h2_esp_spiffs_safe_call_t call = {
        .op = H2_ESP_SPIFFS_MOUNT,
        .max_files = s_spiffs_config.max_files,
        .format_if_mount_failed = s_spiffs_config.format_if_mount_failed ? 1 : 0,
    };
    int base_written = snprintf(
        call.path, sizeof(call.path), "%s", s_spiffs_config.base_path);
    int label_written = snprintf(
        call.second_path,
        sizeof(call.second_path),
        "%s",
        s_spiffs_config.partition_label);
    if (base_written < 0 || (size_t)base_written >= sizeof(call.path) ||
        label_written < 0 ||
        (size_t)label_written >= sizeof(call.second_path)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    int rc = spiffs_run_safe(&call);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    *fs = osal;
    return H2_PAL_FS_OK;
}

int h2_esp_platform_spiffs_fs_deinit(const char *partition_label) {
    if (partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_spiffs_safe_call_t call = {.op = H2_ESP_SPIFFS_UNMOUNT};
    int written = snprintf(call.path, sizeof(call.path), "%s", partition_label);
    if (written < 0 || (size_t)written >= sizeof(call.path)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    return spiffs_run_safe(&call);
}
