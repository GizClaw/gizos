#include "h2_esp_board_private.h"

#include "h2_esp_platform_core.h"

#include <string.h>

static h2_pal_fs_api_t s_board_mount_fs;
static int s_board_mount_fs_ready;

static int mount_h2loader_partition(const char *path);
int h2_esp_board_h2loader_fs_deinit(void);

static int is_h2loader_path(const char *path) {
    return path != NULL &&
        ((strncmp(path, "/dl", 3u) == 0 && (path[3] == '\0' || path[3] == '/')) ||
            (strncmp(path, "/data", 5u) == 0 && (path[5] == '\0' || path[5] == '/')));
}

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t *fs) {
    if (fs == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    int rc = mount_h2loader_partition("/dl");
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = mount_h2loader_partition("/data");
    if (rc != H2_PAL_FS_OK) {
        (void)h2_esp_board_h2loader_fs_deinit();
        return rc;
    }
    return h2_esp_platform_littlefs_fs_use_base_path(fs, "");
}

static int mount_h2loader_partition(const char *path) {
    const char *label;
    h2_esp_platform_littlefs_config_t config;

    if (!is_h2loader_path(path) || strchr(path + 1, '/') != NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    label = path + 1;
    config = (h2_esp_platform_littlefs_config_t){
        .base_path = path,
        .partition_label = label,
        .format_if_mount_failed = true,
    };
    return h2_esp_platform_littlefs_mount(&config);
}

int h2_esp_board_h2loader_fs_deinit(void) {
    int dl_rc = h2_esp_platform_littlefs_fs_deinit("dl");
    int data_rc = h2_esp_platform_littlefs_fs_deinit("data");

    if (dl_rc == H2_PAL_FS_OK && data_rc == H2_PAL_FS_OK) {
        return H2_PAL_FS_OK;
    }
    return H2_PAL_FS_ERR_IO;
}

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
    rc = mount_h2loader_partition(path);
    if (rc == H2_PAL_FS_OK) {
        return H2_PAL_FS_OK;
    }
    rc = ensure_board_mount_fs();
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = board_fs_make_dir_all(&s_board_mount_fs, path);
    return rc == H2_PAL_FS_OK ? H2_PAL_FS_OK : rc;
}

int h2_esp_board_fs_unmount(const char *path) {
    const char *label;

    if (path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    label = path + 1;
    return h2_esp_platform_littlefs_fs_deinit(label);
}

int h2_esp_board_fs_clear(const char *path) {
    const char *label;
    int rc;

    if (path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (strcmp(path, "/data") != 0) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    label = path + 1;
    rc = h2_esp_board_fs_unmount(path);
    if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = h2_esp_platform_littlefs_format(label);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    return mount_h2loader_partition(path);
}

int h2_esp_board_fs_mount_all(void) {
    int rc = h2_esp_board_fs_mount("/dl");
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    return h2_esp_board_fs_mount("/data");
}

int h2_esp_board_fs_unmount_all(void) {
    s_board_mount_fs_ready = 0;
    return h2_esp_board_fs_deinit();
}
