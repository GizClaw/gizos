#include "h2_esp_board_private.h"
#include "h2_esp_platform_core.h"
#include <assert.h>
#include <string.h>

static int dl_live, data_live, fail_data, fail_unmount, fallback;
static int init_calls, deinit_calls;
static int mkdir_ok(void *user, const char *path) {
    (void)user; (void)path;
    return H2_PAL_FS_OK;
}
static const h2_pal_fs_vtable_t fs_ops = {.mkdir = mkdir_ok};
int h2_esp_platform_littlefs_mount(const h2_esp_platform_littlefs_config_t *config) {
    if (fallback) return H2_PAL_FS_ERR_NOT_FOUND;
    if (strcmp(config->partition_label, "data") == 0) {
        if (fail_data) return H2_PAL_FS_ERR_IO;
        data_live = 1;
    } else dl_live = 1;
    return H2_PAL_FS_OK;
}
int h2_esp_platform_littlefs_fs_deinit(const char *label) {
    if (fail_unmount) return H2_PAL_FS_ERR_IO;
    if (strcmp(label, "dl") == 0) dl_live = 0;
    else data_live = 0;
    return H2_PAL_FS_OK;
}
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t *fs, const char *path) {
    (void)path;
    *fs = (h2_pal_fs_api_t){.vtable = &fs_ops};
    return H2_PAL_FS_OK;
}
int h2_esp_platform_littlefs_format(const char *label) {
    (void)label;
    return H2_PAL_FS_OK;
}
int h2_esp_board_fs_init(h2_pal_fs_api_t *fs) {
    ++init_calls;
    if (!fallback) return H2_PAL_FS_ERR_IO;
    *fs = (h2_pal_fs_api_t){.vtable = &fs_ops};
    return H2_PAL_FS_OK;
}
int h2_esp_board_fs_deinit(void) {
    ++deinit_calls;
    if (fallback) return fail_unmount ? H2_PAL_FS_ERR_IO : H2_PAL_FS_OK;
    return h2_esp_board_h2loader_fs_deinit();
}
int main(void) {
    fail_data = 1;
    assert(h2_esp_board_fs_mount_all() == H2_PAL_FS_ERR_IO);
    assert(!dl_live && !data_live && deinit_calls == 1);
    fail_data = 0;
    assert(h2_esp_board_fs_mount_all() == H2_PAL_FS_OK);
    assert(dl_live && data_live);
    assert(h2_esp_board_fs_unmount_all() == H2_PAL_FS_OK);

    fail_data = fail_unmount = 1;
    assert(h2_esp_board_fs_mount_all() == H2_PAL_FS_ERR_IO);
    assert(dl_live && !data_live);
    fail_unmount = 0;
    assert(h2_esp_board_fs_unmount_all() == H2_PAL_FS_OK);
    assert(!dl_live && !data_live);

    fallback = 1;
    init_calls = 0;
    assert(h2_esp_board_fs_mount_all() == H2_PAL_FS_OK);
    assert(init_calls == 1);
    fail_unmount = 1;
    assert(h2_esp_board_fs_unmount_all() == H2_PAL_FS_ERR_IO);
    assert(h2_esp_board_fs_mount("/data") == H2_PAL_FS_OK);
    assert(init_calls == 1);
    fail_unmount = 0;
    assert(h2_esp_board_fs_unmount_all() == H2_PAL_FS_OK);
    assert(h2_esp_board_fs_mount("/data") == H2_PAL_FS_OK);
    assert(init_calls == 2);
    assert(h2_esp_board_fs_unmount_all() == H2_PAL_FS_OK);
    return 0;
}
