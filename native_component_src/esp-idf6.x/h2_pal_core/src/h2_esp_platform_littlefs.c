#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_err.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define H2_ESP_LITTLEFS_PATH_SIZE 192u
#define H2_ESP_LITTLEFS_CHUNK_SIZE 4096u
#define H2_ESP_LITTLEFS_SAFE_STACK_DEPTH 4096u

static StaticSemaphore_t s_littlefs_safe_mutex_storage;
static SemaphoreHandle_t s_littlefs_safe_mutex;
static portMUX_TYPE s_littlefs_safe_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

typedef enum h2_esp_littlefs_safe_op {
    H2_ESP_LITTLEFS_MKDIR = 1,
    H2_ESP_LITTLEFS_OPEN,
    H2_ESP_LITTLEFS_READ,
    H2_ESP_LITTLEFS_SEEK,
    H2_ESP_LITTLEFS_WRITE,
    H2_ESP_LITTLEFS_SYNC,
    H2_ESP_LITTLEFS_CLOSE,
    H2_ESP_LITTLEFS_STAT,
    H2_ESP_LITTLEFS_REMOVE,
    H2_ESP_LITTLEFS_RENAME,
    H2_ESP_LITTLEFS_MOUNT,
    H2_ESP_LITTLEFS_UNMOUNT,
    H2_ESP_LITTLEFS_FORMAT,
} h2_esp_littlefs_safe_op_t;

typedef struct h2_esp_littlefs_safe_call {
    h2_esp_littlefs_safe_op_t op;
    FILE *fp;
    char path[H2_ESP_LITTLEFS_PATH_SIZE];
    char second_path[H2_ESP_LITTLEFS_PATH_SIZE];
    char mode[3];
    uint8_t *data;
    size_t len;
    size_t processed;
    uint64_t position;
    struct stat stat_value;
    int result;
    int error_number;
    int format_if_mount_failed;
} h2_esp_littlefs_safe_call_t;

static void IRAM_ATTR littlefs_safe_callback(void *context) {
    h2_esp_littlefs_safe_call_t *call =
        (h2_esp_littlefs_safe_call_t *)context;
    errno = 0;
    if (call->op == H2_ESP_LITTLEFS_MKDIR) {
        call->result = mkdir(call->path, 0775);
    } else if (call->op == H2_ESP_LITTLEFS_OPEN) {
        call->fp = fopen(call->path, call->mode);
        call->result = call->fp != NULL ? 0 : -1;
    } else if (call->op == H2_ESP_LITTLEFS_READ) {
        call->processed = fread(call->data, 1u, call->len, call->fp);
        call->result = ferror(call->fp) == 0 ? 0 : -1;
    } else if (call->op == H2_ESP_LITTLEFS_SEEK) {
        call->result = call->position <= (uint64_t)LONG_MAX
            ? fseek(call->fp, (long)call->position, SEEK_SET)
            : -1;
        if (call->position > (uint64_t)LONG_MAX) {
            errno = EINVAL;
        }
    } else if (call->op == H2_ESP_LITTLEFS_WRITE) {
        call->processed = fwrite(call->data, 1u, call->len, call->fp);
        call->result = call->processed == call->len ? 0 : -1;
    } else if (call->op == H2_ESP_LITTLEFS_SYNC) {
        call->result = fflush(call->fp);
    } else if (call->op == H2_ESP_LITTLEFS_CLOSE) {
        call->result = fclose(call->fp);
    } else if (call->op == H2_ESP_LITTLEFS_STAT) {
        call->result = stat(call->path, &call->stat_value);
    } else if (call->op == H2_ESP_LITTLEFS_REMOVE) {
        call->result = unlink(call->path);
    } else if (call->op == H2_ESP_LITTLEFS_RENAME) {
        call->result = rename(call->path, call->second_path);
    } else if (call->op == H2_ESP_LITTLEFS_MOUNT) {
        esp_vfs_littlefs_conf_t config = {
            .base_path = call->path,
            .partition_label = call->second_path,
            .format_if_mount_failed = call->format_if_mount_failed != 0,
            .dont_mount = false,
        };
        call->result = esp_vfs_littlefs_register(&config);
    } else if (call->op == H2_ESP_LITTLEFS_UNMOUNT) {
        call->result = esp_vfs_littlefs_unregister(call->path);
    } else if (call->op == H2_ESP_LITTLEFS_FORMAT) {
        call->result = esp_littlefs_format(call->path);
    } else {
        call->result = -1;
        errno = EINVAL;
    }
    call->error_number = errno;
}

static SemaphoreHandle_t littlefs_safe_mutex(void) {
    portENTER_CRITICAL(&s_littlefs_safe_mutex_init_lock);
    if (s_littlefs_safe_mutex == NULL) {
        s_littlefs_safe_mutex =
            xSemaphoreCreateMutexStatic(&s_littlefs_safe_mutex_storage);
    }
    portEXIT_CRITICAL(&s_littlefs_safe_mutex_init_lock);
    return s_littlefs_safe_mutex;
}

static int littlefs_run_safe(h2_esp_littlefs_safe_call_t *call) {
    SemaphoreHandle_t mutex = littlefs_safe_mutex();
    if (mutex == NULL ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_FS_ERR_IO;
    }
    h2_pal_result_t rc = h2_esp_platform_safe_call(
        littlefs_safe_callback,
        call,
        sizeof(*call),
        H2_ESP_LITTLEFS_SAFE_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    return rc == H2_PAL_OK ? H2_PAL_FS_OK : rc;
}

typedef struct h2_esp_platform_littlefs_context {
    const h2_esp_platform_littlefs_config_t *config;
} h2_esp_platform_littlefs_context_t;

typedef struct h2_esp_platform_littlefs_file {
    FILE *fp;
} h2_esp_platform_littlefs_file_t;

static int littlefs_mkdir(void *user, const char *path);
static int littlefs_open(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file);
static int littlefs_read(void *user, h2_pal_fs_file_t *raw_file, void *data, size_t len, size_t *out_read);
static int littlefs_seek(
    void *user,
    h2_pal_fs_file_t *raw_file,
    uint64_t position);
static int littlefs_write(
    void *user,
    h2_pal_fs_file_t *raw_file,
    const void *data,
    size_t len,
    size_t *out_written);
static int littlefs_sync(void *user, h2_pal_fs_file_t *raw_file);
static int littlefs_close(void *user, h2_pal_fs_file_t *raw_file);
static int littlefs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat);
static int littlefs_remove(void *user, const char *path);
static int littlefs_rename(void *user, const char *old_path, const char *new_path);

static h2_esp_platform_littlefs_config_t s_littlefs_config;
static h2_esp_platform_littlefs_context_t s_littlefs_context = {
    .config = &s_littlefs_config,
};

static const h2_pal_fs_vtable_t s_littlefs_api_vtable = {
    .mkdir = littlefs_mkdir,
    .open = littlefs_open,
    .read = littlefs_read,
    .seek = littlefs_seek,
    .write = littlefs_write,
    .sync = littlefs_sync,
    .close = littlefs_close,
    .stat = littlefs_stat,
    .remove = littlefs_remove,
    .rename = littlefs_rename,
    };

static const h2_pal_fs_api_t s_littlefs_api = {
    .user = &s_littlefs_context,
    .vtable = &s_littlefs_api_vtable,
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
    case EEXIST:
        return H2_PAL_FS_OK;
    case EINVAL:
        return H2_PAL_FS_ERR_INVALID_ARG;
    default:
        return H2_PAL_FS_ERR_IO;
    }
}

static int map_errno_value(int error_number, int fallback) {
    errno = error_number;
    return map_errno(fallback);
}

static int translate_path(const h2_esp_platform_littlefs_context_t *ctx, const char *path, char *out, size_t out_len) {
    int len;

    if (ctx == NULL || ctx->config == NULL || ctx->config->base_path == NULL ||
        path == NULL || out == NULL || out_len == 0u || path[0] != '/') {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    for (const char *cursor = path; *cursor != '\0';) {
        const char *start;
        size_t component_len;

        while (*cursor == '/') {
            ++cursor;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        component_len = (size_t)(cursor - start);
        if ((component_len == 1u && start[0] == '.') ||
            (component_len == 2u && start[0] == '.' && start[1] == '.')) {
            return H2_PAL_FS_ERR_INVALID_ARG;
        }
    }
    len = snprintf(out, out_len, "%s%s", ctx->config->base_path, path);
    if (len < 0 || (size_t)len >= out_len) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    return H2_PAL_FS_OK;
}

static int littlefs_mkdir(void *user, const char *path) {
    h2_esp_platform_littlefs_context_t *ctx = (h2_esp_platform_littlefs_context_t *)user;
    char real_path[192];
    int rc = translate_path(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_MKDIR};
    (void)snprintf(call.path, sizeof(call.path), "%s", real_path);
    rc = littlefs_run_safe(&call);
    return rc != H2_PAL_FS_OK || call.result == 0
        ? rc
        : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
}

static int littlefs_open(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file) {
    h2_esp_platform_littlefs_context_t *ctx = (h2_esp_platform_littlefs_context_t *)user;
    char real_path[192];
    h2_esp_platform_littlefs_file_t *file;
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

    rc = translate_path(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    file = (h2_esp_platform_littlefs_file_t *)calloc(1u, sizeof(*file));
    if (file == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_OPEN};
    (void)snprintf(call.path, sizeof(call.path), "%s", real_path);
    memcpy(call.mode, fmode, strlen(fmode) + 1u);
    rc = littlefs_run_safe(&call);
    file->fp = call.fp;
    if (rc != H2_PAL_FS_OK || file->fp == NULL) {
        free(file);
        return rc != H2_PAL_FS_OK
            ? rc
            : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
    }
    *out_file = (h2_pal_fs_file_t *)file;
    return H2_PAL_FS_OK;
}

static int littlefs_read(void *user, h2_pal_fs_file_t *raw_file, void *data, size_t len, size_t *out_read) {
    h2_esp_platform_littlefs_file_t *file = (h2_esp_platform_littlefs_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL || data == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    if (len == 0u) {
        return H2_PAL_FS_OK;
    }
    uint8_t *internal_data = NULL;
    size_t safe_capacity = 0u;
    h2_pal_result_t safe_rc = h2_esp_platform_safe_io_acquire(
        &internal_data, &safe_capacity);
    if (safe_rc != H2_PAL_OK ||
        internal_data == NULL || safe_capacity == 0u) {
        if (safe_rc == H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
        }
        return H2_PAL_FS_ERR_IO;
    }
    const size_t internal_capacity =
        len < H2_ESP_LITTLEFS_CHUNK_SIZE ? len : H2_ESP_LITTLEFS_CHUNK_SIZE;
    if (internal_capacity > safe_capacity) {
        h2_esp_platform_safe_io_release();
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int rc = H2_PAL_FS_OK;
    while (*out_read < len) {
        h2_esp_littlefs_safe_call_t call = {
            .op = H2_ESP_LITTLEFS_READ,
            .fp = file->fp,
            .data = internal_data,
            .len = len - *out_read > internal_capacity
                ? internal_capacity
                : len - *out_read,
        };
        rc = littlefs_run_safe(&call);
        if (rc != H2_PAL_FS_OK || call.result != 0) {
            rc = rc != H2_PAL_FS_OK
                ? rc
                : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
            break;
        }
        memcpy((uint8_t *)data + *out_read, internal_data, call.processed);
        *out_read += call.processed;
        if (call.processed < call.len) {
            break;
        }
    }
    h2_esp_platform_safe_io_release();
    return rc;
}

static int littlefs_seek(
    void *user,
    h2_pal_fs_file_t *raw_file,
    uint64_t position) {
    h2_esp_platform_littlefs_file_t *file =
        (h2_esp_platform_littlefs_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL || position > (uint64_t)LONG_MAX) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_littlefs_safe_call_t call = {
        .op = H2_ESP_LITTLEFS_SEEK,
        .fp = file->fp,
        .position = position,
    };
    int rc = littlefs_run_safe(&call);
    return rc != H2_PAL_FS_OK || call.result == 0
        ? rc
        : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
}

static int littlefs_write(void *user, h2_pal_fs_file_t *raw_file, const void *data, size_t len, size_t *out_written) {
    h2_esp_platform_littlefs_file_t *file = (h2_esp_platform_littlefs_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL || (data == NULL && len != 0u) || out_written == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    if (len == 0u) {
        return H2_PAL_FS_OK;
    }
    uint8_t *internal_data = NULL;
    size_t internal_capacity = 0u;
    h2_pal_result_t safe_rc = h2_esp_platform_safe_io_acquire(
        &internal_data, &internal_capacity);
    if (safe_rc != H2_PAL_OK ||
        internal_data == NULL) {
        if (safe_rc == H2_PAL_OK) {
            h2_esp_platform_safe_io_release();
        }
        return H2_PAL_FS_ERR_IO;
    }
    if (internal_capacity < H2_ESP_LITTLEFS_CHUNK_SIZE) {
        h2_esp_platform_safe_io_release();
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int rc = H2_PAL_FS_OK;
    while (*out_written < len) {
        h2_esp_littlefs_safe_call_t call = {
            .op = H2_ESP_LITTLEFS_WRITE,
            .fp = file->fp,
            .data = internal_data,
            .len = len - *out_written > H2_ESP_LITTLEFS_CHUNK_SIZE
                ? H2_ESP_LITTLEFS_CHUNK_SIZE
                : len - *out_written,
        };
        memcpy(internal_data, (const uint8_t *)data + *out_written, call.len);
        rc = littlefs_run_safe(&call);
        *out_written += call.processed;
        if (rc != H2_PAL_FS_OK || call.result != 0) {
            rc = rc != H2_PAL_FS_OK
                ? rc
                : map_errno_value(call.error_number, H2_PAL_FS_ERR_NO_SPACE);
            break;
        }
    }
    h2_esp_platform_safe_io_release();
    return rc;
}

static int littlefs_sync(void *user, h2_pal_fs_file_t *raw_file) {
    h2_esp_platform_littlefs_file_t *file = (h2_esp_platform_littlefs_file_t *)raw_file;
    (void)user;

    if (file == NULL || file->fp == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_littlefs_safe_call_t call = {
        .op = H2_ESP_LITTLEFS_SYNC,
        .fp = file->fp,
    };
    int safe_rc = littlefs_run_safe(&call);
    return safe_rc != H2_PAL_FS_OK || call.result == 0
        ? safe_rc
        : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
}

static int littlefs_close(void *user, h2_pal_fs_file_t *raw_file) {
    h2_esp_platform_littlefs_file_t *file = (h2_esp_platform_littlefs_file_t *)raw_file;
    int rc = H2_PAL_FS_OK;
    (void)user;

    if (file == NULL || file->fp == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_littlefs_safe_call_t call = {
        .op = H2_ESP_LITTLEFS_CLOSE,
        .fp = file->fp,
    };
    rc = littlefs_run_safe(&call);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (call.result != 0) {
        rc = map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
    }
    free(file);
    return rc;
}

static int littlefs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
    h2_esp_platform_littlefs_context_t *ctx = (h2_esp_platform_littlefs_context_t *)user;
    char real_path[192];
    struct stat st;
    int rc;

    if (out_stat == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    rc = translate_path(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_STAT};
    (void)snprintf(call.path, sizeof(call.path), "%s", real_path);
    rc = littlefs_run_safe(&call);
    if (rc != H2_PAL_FS_OK || call.result != 0) {
        return rc != H2_PAL_FS_OK
            ? rc
            : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
    }
    st = call.stat_value;
    out_stat->size = (uint64_t)st.st_size;
    out_stat->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return H2_PAL_FS_OK;
}

static int littlefs_remove(void *user, const char *path) {
    h2_esp_platform_littlefs_context_t *ctx = (h2_esp_platform_littlefs_context_t *)user;
    char real_path[192];
    int rc = translate_path(ctx, path, real_path, sizeof(real_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_REMOVE};
    (void)snprintf(call.path, sizeof(call.path), "%s", real_path);
    rc = littlefs_run_safe(&call);
    return rc != H2_PAL_FS_OK || call.result == 0
        ? rc
        : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
}

static int littlefs_rename(void *user, const char *old_path, const char *new_path) {
    h2_esp_platform_littlefs_context_t *ctx = (h2_esp_platform_littlefs_context_t *)user;
    char real_old_path[192];
    char real_new_path[192];
    int rc = translate_path(ctx, old_path, real_old_path, sizeof(real_old_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = translate_path(ctx, new_path, real_new_path, sizeof(real_new_path));
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_RENAME};
    (void)snprintf(call.path, sizeof(call.path), "%s", real_old_path);
    (void)snprintf(
        call.second_path, sizeof(call.second_path), "%s", real_new_path);
    rc = littlefs_run_safe(&call);
    return rc != H2_PAL_FS_OK || call.result == 0
        ? rc
        : map_errno_value(call.error_number, H2_PAL_FS_ERR_IO);
}

int h2_esp_platform_littlefs_mount(const h2_esp_platform_littlefs_config_t *config) {
    esp_err_t err;

    if (config == NULL || config->base_path == NULL || config->partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (esp_littlefs_mounted(config->partition_label)) {
        return H2_PAL_FS_OK;
    }

    h2_esp_littlefs_safe_call_t call = {
        .op = H2_ESP_LITTLEFS_MOUNT,
        .format_if_mount_failed = config->format_if_mount_failed ? 1 : 0,
    };
    int base_len =
        snprintf(call.path, sizeof(call.path), "%s", config->base_path);
    int label_len = snprintf(
        call.second_path,
        sizeof(call.second_path),
        "%s",
        config->partition_label);
    if (base_len < 0 || (size_t)base_len >= sizeof(call.path) ||
        label_len < 0 || (size_t)label_len >= sizeof(call.second_path)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    int safe_rc = littlefs_run_safe(&call);
    if (safe_rc != H2_PAL_FS_OK) {
        return safe_rc;
    }
    err = call.result;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return H2_PAL_FS_ERR_IO;
    }
    return H2_PAL_FS_OK;
}

int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t *fs, const char *base_path) {
    if (fs == NULL || base_path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    s_littlefs_config.base_path = base_path;
    s_littlefs_config.partition_label = "";
    s_littlefs_config.format_if_mount_failed = false;
    *fs = s_littlefs_api;
    return H2_PAL_FS_OK;
}

int h2_esp_platform_littlefs_format(const char *partition_label) {
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_FORMAT};
    int label_len;
    int safe_rc;

    if (partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    label_len = snprintf(call.path, sizeof(call.path), "%s", partition_label);
    if (label_len < 0 || (size_t)label_len >= sizeof(call.path)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    safe_rc = littlefs_run_safe(&call);
    if (safe_rc != H2_PAL_FS_OK) {
        return safe_rc;
    }
    return call.result == ESP_OK ? H2_PAL_FS_OK : H2_PAL_FS_ERR_IO;
}

int h2_esp_platform_littlefs_fs_init(h2_pal_fs_api_t *fs, const h2_esp_platform_littlefs_config_t *config) {
    int rc;

    if (fs == NULL || config == NULL || config->base_path == NULL || config->partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    s_littlefs_config = *config;
    rc = h2_esp_platform_littlefs_mount(config);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    *fs = s_littlefs_api;
    return H2_PAL_FS_OK;
}

int h2_esp_platform_littlefs_fs_deinit(const char *partition_label) {
    if (partition_label == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_esp_littlefs_safe_call_t call = {.op = H2_ESP_LITTLEFS_UNMOUNT};
    int label_len =
        snprintf(call.path, sizeof(call.path), "%s", partition_label);
    if (label_len < 0 || (size_t)label_len >= sizeof(call.path)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    int safe_rc = littlefs_run_safe(&call);
    if (safe_rc != H2_PAL_FS_OK) {
        return safe_rc;
    }
    esp_err_t err = call.result;
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE ? H2_PAL_FS_OK : H2_PAL_FS_ERR_IO;
}
