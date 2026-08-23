#include "h2_lvgl_fs.h"
#include "h2_lvgl_fs_internal.h"

#include "lvgl.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_LVGL_FS_PATH_CAPACITY 256u
#define H2_LVGL_FS_SKIP_BUFFER_SIZE 512u

typedef struct h2_lvgl_fs_file {
    struct h2_lvgl_fs *adapter;
    h2_pal_fs_file_t *file;
    char path[H2_LVGL_FS_PATH_CAPACITY];
    uint32_t position;
} h2_lvgl_fs_file_t;

struct h2_lvgl_fs {
    const h2_pal_fs_api_t *fs;
    const h2_pal_mem_api_t *allocator;
    const char *root;
    lv_fs_drv_t driver;
};

static bool lvgl_fs_ready(lv_fs_drv_t *driver) {
    h2_lvgl_fs_t *adapter = driver != NULL ? driver->user_data : NULL;
    return adapter != NULL && adapter->fs != NULL;
}

static lv_fs_res_t lvgl_fs_reopen(h2_lvgl_fs_file_t *file) {
    if (file->file != NULL) {
        h2_pal_result_t close_rc =
            h2_pal_fs_close(file->adapter->fs, file->file);
        file->file = NULL;
        if (close_rc != H2_PAL_OK) {
            return LV_FS_RES_UNKNOWN;
        }
    }
    return h2_pal_fs_open(file->adapter->fs, file->path,
                          H2_PAL_FS_OPEN_READ, &file->file) == H2_PAL_OK
               ? LV_FS_RES_OK
               : LV_FS_RES_UNKNOWN;
}

static void *lvgl_fs_open(lv_fs_drv_t *driver, const char *path,
                          lv_fs_mode_t mode) {
    h2_lvgl_fs_t *adapter = driver != NULL ? driver->user_data : NULL;
    if (adapter == NULL || path == NULL || mode != LV_FS_MODE_RD) {
        return NULL;
    }
    h2_lvgl_fs_file_t *file =
        h2_pal_mem_alloc(adapter->allocator, sizeof(*file));
    if (file == NULL) {
        return NULL;
    }
    memset(file, 0, sizeof(*file));
    file->adapter = adapter;
    int written;
    size_t root_len = strlen(adapter->root);
    if (strcmp(adapter->root, "/") == 0) {
        written = snprintf(file->path, sizeof(file->path), "%s%s",
                           path[0] == '/' ? "" : "/", path);
    } else if (adapter->root[root_len - 1u] == '/') {
        written = snprintf(file->path, sizeof(file->path), "%s%s",
                           adapter->root, path[0] == '/' ? path + 1 : path);
    } else if (path[0] == '/') {
        written = snprintf(file->path, sizeof(file->path), "%s%s",
                           adapter->root, path);
    } else {
        written = snprintf(file->path, sizeof(file->path), "%s/%s",
                           adapter->root, path);
    }
    if (written <= 0 || (size_t)written >= sizeof(file->path) ||
        lvgl_fs_reopen(file) != LV_FS_RES_OK) {
        h2_pal_mem_free(adapter->allocator, file);
        return NULL;
    }
    return file;
}

static lv_fs_res_t lvgl_fs_close(lv_fs_drv_t *driver, void *raw_file) {
    (void)driver;
    h2_lvgl_fs_file_t *file = raw_file;
    if (file == NULL || file->adapter == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    h2_pal_result_t rc = file->file != NULL
                             ? h2_pal_fs_close(file->adapter->fs, file->file)
                             : H2_PAL_OK;
    h2_pal_mem_free(file->adapter->allocator, file);
    return rc == H2_PAL_OK ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t lvgl_fs_read(lv_fs_drv_t *driver, void *raw_file,
                                void *buffer, uint32_t bytes_to_read,
                                uint32_t *out_bytes_read) {
    (void)driver;
    h2_lvgl_fs_file_t *file = raw_file;
    if (file == NULL || buffer == NULL || out_bytes_read == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    *out_bytes_read = 0u;
    size_t bytes_read = 0u;
    h2_pal_result_t rc = h2_pal_fs_read(file->adapter->fs, file->file, buffer,
                                        bytes_to_read, &bytes_read);
    if (rc != H2_PAL_OK || bytes_read > UINT32_MAX - file->position) {
        return LV_FS_RES_UNKNOWN;
    }
    file->position += (uint32_t)bytes_read;
    *out_bytes_read = (uint32_t)bytes_read;
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_skip_to(h2_lvgl_fs_file_t *file,
                                   uint32_t target) {
    h2_pal_result_t seek_rc =
        h2_pal_fs_seek(file->adapter->fs, file->file, target);
    if (seek_rc == H2_PAL_OK) {
        file->position = target;
        return LV_FS_RES_OK;
    }
    if (seek_rc != H2_PAL_FS_ERR_UNSUPPORTED) {
        return LV_FS_RES_UNKNOWN;
    }
    if (target < file->position) {
        if (lvgl_fs_reopen(file) != LV_FS_RES_OK) {
            return LV_FS_RES_UNKNOWN;
        }
        file->position = 0u;
    }
    uint8_t scratch[H2_LVGL_FS_SKIP_BUFFER_SIZE];
    while (file->position < target) {
        uint32_t remaining = target - file->position;
        size_t request = remaining < sizeof(scratch) ? remaining
                                                     : sizeof(scratch);
        size_t bytes_read = 0u;
        h2_pal_result_t rc = h2_pal_fs_read(file->adapter->fs, file->file,
                                            scratch, request, &bytes_read);
        if (rc != H2_PAL_OK || bytes_read == 0u ||
            bytes_read > UINT32_MAX - file->position) {
            return LV_FS_RES_UNKNOWN;
        }
        file->position += (uint32_t)bytes_read;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_seek(lv_fs_drv_t *driver, void *raw_file,
                                uint32_t position, lv_fs_whence_t whence) {
    (void)driver;
    h2_lvgl_fs_file_t *file = raw_file;
    if (file == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    uint32_t target = position;
    if (whence == LV_FS_SEEK_CUR) {
        if (!h2_lvgl_fs_relative_target(file->position, position, &target)) {
            return LV_FS_RES_INV_PARAM;
        }
    } else if (whence == LV_FS_SEEK_END) {
        h2_pal_fs_stat_t stat;
        if (h2_pal_fs_stat(file->adapter->fs, file->path, &stat) != H2_PAL_OK) {
            return LV_FS_RES_UNKNOWN;
        }
        if (!h2_lvgl_fs_relative_target(stat.size, position, &target)) {
            return LV_FS_RES_INV_PARAM;
        }
    } else if (whence != LV_FS_SEEK_SET) {
        return LV_FS_RES_INV_PARAM;
    }
    return lvgl_fs_skip_to(file, target);
}

static lv_fs_res_t lvgl_fs_tell(lv_fs_drv_t *driver, void *raw_file,
                                uint32_t *out_position) {
    (void)driver;
    h2_lvgl_fs_file_t *file = raw_file;
    if (file == NULL || out_position == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    *out_position = file->position;
    return LV_FS_RES_OK;
}

h2_pal_result_t h2_lvgl_fs_register(const h2_lvgl_fs_config_t *config,
                                    h2_lvgl_fs_t **out_fs) {
    if (out_fs == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_fs = NULL;
    if (config == NULL || config->fs == NULL || config->allocator == NULL ||
        config->root == NULL || config->root[0] != '/' ||
        config->drive_letter == '\0' ||
        lv_fs_get_drv(config->drive_letter) != NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_lvgl_fs_t *adapter =
        h2_pal_mem_alloc(config->allocator, sizeof(*adapter));
    if (adapter == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->fs = config->fs;
    adapter->allocator = config->allocator;
    adapter->root = config->root;
    lv_fs_drv_init(&adapter->driver);
    adapter->driver.letter = config->drive_letter;
    adapter->driver.cache_size = config->cache_size;
    adapter->driver.ready_cb = lvgl_fs_ready;
    adapter->driver.open_cb = lvgl_fs_open;
    adapter->driver.close_cb = lvgl_fs_close;
    adapter->driver.read_cb = lvgl_fs_read;
    adapter->driver.seek_cb = lvgl_fs_seek;
    adapter->driver.tell_cb = lvgl_fs_tell;
    adapter->driver.user_data = adapter;
    lv_fs_drv_register(&adapter->driver);
    if (lv_fs_get_drv(config->drive_letter) != &adapter->driver) {
        h2_pal_mem_free(config->allocator, adapter);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_fs = adapter;
    return H2_PAL_OK;
}

void h2_lvgl_fs_release(h2_lvgl_fs_t *fs) {
    if (fs == NULL) {
        return;
    }
    h2_pal_mem_free(fs->allocator, fs);
}
