#include "h2_bk_h2loader.h"

#include "driver/pwr_clk.h"
#include "driver/sd_card.h"
#include "driver/sdio_host.h"
#include "diskio.h"
#include "ff.h"
#include "gpio_driver.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "os/mem.h"
#include "os/os.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_FS_PATH_MAX 192u
#define H2_BK_SD_MOUNT_ATTEMPTS 5u
#define H2_BK_SD_POWER_OFF_MS 1000u
#define H2_BK_SD_POWER_ON_MS 2000u

struct h2_pal_fs_file {
    FIL file;
};

static FATFS s_fs;
static int s_mounted;

static void sd_power_set(gpio_output_state_e state) {
#if CONFIG_SDCARD_POWER_GPIO_CTRL
    (void)bk_pm_module_vote_ctrl_external_ldo(
        GPIO_CTRL_LDO_MODULE_SDIO,
        CONFIG_LDO3V3_CTRL_GPIO,
        state);
#else
    (void)state;
#endif
}

static void sd_map_pins(void) {
#if CONFIG_SDIO_4LINES_EN
    (void)gpio_sdio_sel(GPIO_SDIO_MAP_MODE0);
#else
    (void)gpio_sdio_one_line_sel(GPIO_SDIO_MAP_MODE0);
#endif
}

void h2_bk_h2loader_release_sd_storage(void) {
    (void)f_mount(NULL, H2_BK_H2LOADER_FATFS_DRIVE, 0);
    (void)disk_uninitialize(DISK_NUMBER_SDIO_SD);
    s_mounted = 0;
}

void h2_bk_h2loader_prepare_sd_storage(void) {
    h2_bk_h2loader_release_sd_storage();
    (void)bk_sd_card_deinit();
    (void)bk_sdio_host_driver_deinit();
    sd_power_set(GPIO_OUTPUT_STATE_LOW);
    rtos_delay_milliseconds(H2_BK_SD_POWER_OFF_MS);
    sd_map_pins();
    sd_power_set(GPIO_OUTPUT_STATE_HIGH);
    rtos_delay_milliseconds(H2_BK_SD_POWER_ON_MS);
    (void)bk_sdio_host_driver_init();
}

static int map_fresult(FRESULT fr) {
    switch (fr) {
    case FR_OK:
        return H2_PAL_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return H2_PAL_ERR_NOT_FOUND;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return H2_PAL_ERR_IO;
    case FR_NOT_ENOUGH_CORE:
        return H2_PAL_ERR_NO_MEMORY;
    case FR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    default:
        return H2_PAL_ERR_IO;
    }
}

static int append_suffix(char *out, size_t out_len, const char *root, const char *suffix) {
    int written;

    if (out == NULL || root == NULL || suffix == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    written = snprintf(out, out_len, "%s%s", root, suffix);
    if (written < 0 || (size_t)written >= out_len) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    return H2_PAL_OK;
}

static int translate_path(const char *path, char *out, size_t out_len) {
    if (path == NULL || out == NULL || out_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(path, "/dl") == 0) {
        return append_suffix(out, out_len, H2_BK_H2LOADER_SD_DL_ROOT, "");
    }
    if (strncmp(path, "/dl/", 4u) == 0) {
        return append_suffix(out, out_len, H2_BK_H2LOADER_SD_DL_ROOT, path + 3u);
    }
    if (strcmp(path, "/data") == 0) {
        return append_suffix(out, out_len, H2_BK_H2LOADER_SD_DATA_ROOT, "");
    }
    if (strncmp(path, "/data/", 6u) == 0) {
        return append_suffix(out, out_len, H2_BK_H2LOADER_SD_DATA_ROOT, path + 5u);
    }
    return H2_PAL_ERR_INVALID_ARG;
}

static int ensure_dir(const char *path) {
    FRESULT fr = f_mkdir(path);
    return fr == FR_OK || fr == FR_EXIST ? H2_PAL_OK : map_fresult(fr);
}

static int fs_mkdir(void *user, const char *path) {
    char mapped[H2_BK_FS_PATH_MAX];
    int rc;

    (void)user;
    rc = translate_path(path, mapped, sizeof(mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return ensure_dir(mapped);
}

static int fs_open(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file) {
    char mapped[H2_BK_FS_PATH_MAX];
    BYTE fatfs_mode;
    h2_pal_fs_file_t *file;
    int rc;
    FRESULT fr;

    (void)user;
    if (out_file == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    rc = translate_path(path, mapped, sizeof(mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    switch (mode) {
    case H2_PAL_FS_OPEN_WRITE_TRUNCATE:
        fatfs_mode = FA_WRITE | FA_CREATE_ALWAYS;
        break;
    case H2_PAL_FS_OPEN_READ:
        fatfs_mode = FA_READ;
        break;
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
    file = (h2_pal_fs_file_t *)os_malloc(sizeof(*file));
    if (file == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    fr = f_open(&file->file, mapped, fatfs_mode);
    if (fr != FR_OK) {
        os_free(file);
        return map_fresult(fr);
    }
    *out_file = file;
    return H2_PAL_OK;
}

static int fs_read(void *user, h2_pal_fs_file_t *file, void *data, size_t len, size_t *out_read) {
    UINT n = 0u;
    FRESULT fr;

    (void)user;
    if (file == NULL || (data == NULL && len != 0u) || out_read == NULL || len > UINT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    fr = f_read(&file->file, data, (UINT)len, &n);
    if (fr != FR_OK) {
        return map_fresult(fr);
    }
    *out_read = (size_t)n;
    return H2_PAL_OK;
}

static int fs_write(void *user, h2_pal_fs_file_t *file, const void *data, size_t len, size_t *out_written) {
    UINT n = 0u;
    FRESULT fr;

    (void)user;
    if (file == NULL || (data == NULL && len != 0u) || out_written == NULL || len > UINT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    fr = f_write(&file->file, data, (UINT)len, &n);
    if (fr != FR_OK) {
        return map_fresult(fr);
    }
    *out_written = (size_t)n;
    return H2_PAL_OK;
}

static int fs_sync(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    if (file == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return map_fresult(f_sync(&file->file));
}

static int fs_close(void *user, h2_pal_fs_file_t *file) {
    FRESULT fr;

    (void)user;
    if (file == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    fr = f_close(&file->file);
    os_free(file);
    return map_fresult(fr);
}

static int fs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
    char mapped[H2_BK_FS_PATH_MAX];
    FILINFO info;
    int rc;
    FRESULT fr;

    (void)user;
    if (out_stat == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    rc = translate_path(path, mapped, sizeof(mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    fr = f_stat(mapped, &info);
    if (fr != FR_OK) {
        return map_fresult(fr);
    }
    out_stat->size = (uint64_t)info.fsize;
    out_stat->is_dir = (info.fattrib & AM_DIR) != 0u;
    return H2_PAL_OK;
}

static int fs_remove(void *user, const char *path) {
    char mapped[H2_BK_FS_PATH_MAX];
    int rc;

    (void)user;
    rc = translate_path(path, mapped, sizeof(mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return map_fresult(f_unlink(mapped));
}

static int fs_rename(void *user, const char *old_path, const char *new_path) {
    char old_mapped[H2_BK_FS_PATH_MAX];
    char new_mapped[H2_BK_FS_PATH_MAX];
    int rc;

    (void)user;
    rc = translate_path(old_path, old_mapped, sizeof(old_mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = translate_path(new_path, new_mapped, sizeof(new_mapped));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return map_fresult(f_rename(old_mapped, new_mapped));
}

static int join_fatfs_path(char *out, size_t out_len, const char *dir, const char *name) {
    int written;

    if (out == NULL || dir == NULL || name == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    written = snprintf(out, out_len, "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= out_len) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    return H2_PAL_OK;
}

static int clear_dir_children(const char *dir_path) {
    DIR dir;
    FILINFO info;
    FRESULT fr;
    int rc = H2_PAL_OK;

    fr = f_opendir(&dir, dir_path);
    if (fr != FR_OK) {
        return map_fresult(fr);
    }

    for (;;) {
        char child[H2_BK_FS_PATH_MAX];

        fr = f_readdir(&dir, &info);
        if (fr != FR_OK) {
            rc = map_fresult(fr);
            break;
        }
        if (info.fname[0] == '\0') {
            break;
        }
        if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0) {
            continue;
        }

        rc = join_fatfs_path(child, sizeof(child), dir_path, info.fname);
        if (rc != H2_PAL_OK) {
            break;
        }
        if ((info.fattrib & AM_DIR) != 0u) {
            rc = clear_dir_children(child);
            if (rc != H2_PAL_OK) {
                break;
            }
        }
        rc = map_fresult(f_unlink(child));
        if (rc != H2_PAL_OK) {
            break;
        }
    }

    fr = f_closedir(&dir);
    if (rc == H2_PAL_OK && fr != FR_OK) {
        rc = map_fresult(fr);
    }
    return rc;
}

int h2_bk_h2loader_mount_file_point(void *user, const char *path) {
    FRESULT fr;
    int rc;

    (void)user;
    if (path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        for (uint32_t attempt = 0; attempt < H2_BK_SD_MOUNT_ATTEMPTS; ++attempt) {
            h2_bk_h2loader_prepare_sd_storage();
            fr = f_mount(&s_fs, H2_BK_H2LOADER_FATFS_DRIVE, 1);
            if (fr == FR_OK) {
                break;
            }
            os_printf(
                "H2_BK_SD_MOUNT_RETRY attempt=%u fatfs=%d rc=%d\r\n",
                (unsigned)(attempt + 1u),
                (int)fr,
                map_fresult(fr));
        }
        if (fr != FR_OK) {
            os_printf("H2_BK_SD_MOUNT_FAIL fatfs=%d rc=%d\r\n", (int)fr, map_fresult(fr));
            return map_fresult(fr);
        }
        rc = ensure_dir(H2_BK_H2LOADER_SD_ROOT);
        if (rc != H2_PAL_OK) {
            (void)f_mount(NULL, H2_BK_H2LOADER_FATFS_DRIVE, 0);
            return rc;
        }
        s_mounted = 1;
    }
    if (strcmp(path, "/dl") == 0) {
        return ensure_dir(H2_BK_H2LOADER_SD_DL_ROOT);
    }
    if (strcmp(path, "/data") == 0) {
        return ensure_dir(H2_BK_H2LOADER_SD_DATA_ROOT);
    }
    return H2_PAL_ERR_INVALID_ARG;
}

int h2_bk_h2loader_clear_data(void *user, const char *path) {
    int rc;

    (void)user;
    if (path == NULL || strcmp(path, "/data") != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_bk_h2loader_mount_file_point(NULL, "/data");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return clear_dir_children(H2_BK_H2LOADER_SD_DATA_ROOT);
}

int h2_bk_h2loader_sd_fs_init(h2_pal_fs_api_t *fs) {
    static const h2_pal_fs_vtable_t api_vtable = {
        .mkdir = fs_mkdir,
        .open = fs_open,
        .read = fs_read,
        .write = fs_write,
        .sync = fs_sync,
        .close = fs_close,
        .stat = fs_stat,
        .remove = fs_remove,
        .rename = fs_rename,
    };

static const h2_pal_fs_api_t api = {
    .user = NULL,
    .vtable = &api_vtable,
};

    if (fs == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *fs = api;
    return H2_PAL_OK;
}
