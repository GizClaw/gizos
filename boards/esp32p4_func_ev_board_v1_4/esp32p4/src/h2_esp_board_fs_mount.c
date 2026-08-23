#include "h2_esp_board_private.h"

#include <string.h>

static h2_pal_fs_api_t s_board_mount_fs;
static int s_board_mount_fs_ready;

static int ensure_board_mount_fs(void) {
    int rc;

    if (s_board_mount_fs_ready) {
        return H2_PAL_FS_OK;
    }
    rc = h2_esp_board_fs_init(&s_board_mount_fs);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    s_board_mount_fs_ready = 1;
    return H2_PAL_FS_OK;
}

static int board_fs_make_dir_all(h2_pal_fs_api_t *fs, const char *path) {
    char current[384];
    size_t len;

    if (fs == NULL || path == NULL || path[0] == '\0') {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    len = strlen(path);
    if (len >= sizeof(current)) {
        return H2_PAL_FS_ERR_NO_SPACE;
    }
    memcpy(current, path, len + 1u);
    for (size_t i = 1u; i < len; ++i) {
        if (current[i] != '/') {
            continue;
        }
        current[i] = '\0';
        if (current[0] != '\0') {
            int rc = h2_pal_fs_mkdir(fs, current);
            if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_UNSUPPORTED) {
                return rc;
            }
        }
        current[i] = '/';
    }
    return h2_pal_fs_mkdir(fs, current);
}

int h2_esp_board_fs_mount(const char *path) {
    int rc;

    if (path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    rc = ensure_board_mount_fs();
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = board_fs_make_dir_all(&s_board_mount_fs, path);
    return rc == H2_PAL_FS_OK ? H2_PAL_FS_OK : rc;
}

int h2_esp_board_fs_unmount(const char *path) {
    if (path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    return H2_PAL_FS_OK;
}

int h2_esp_board_fs_mount_all(void) {
    int rc = h2_esp_board_fs_mount("/dl");
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    return h2_esp_board_fs_mount("/data");
}

int h2_esp_board_fs_unmount_all(void) {
    if (!s_board_mount_fs_ready) {
        return H2_PAL_FS_OK;
    }
    s_board_mount_fs_ready = 0;
    return h2_esp_board_fs_deinit();
}
