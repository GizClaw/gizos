#include "h2_partial_update_smoke.h"

#include <stdio.h>
#include <string.h>

#define H2_PARTIAL_UPDATE_DATA_PATH "/data/version.txt"

int h2_partial_update_smoke_run(
    h2_runtime_t *runtime,
    const h2_partial_update_smoke_config_t *config) {
    h2_pal_fs_file_t *file = NULL;
    char data_generation[32];
    size_t read_len = 0u;
    int rc;

    if (runtime == NULL || runtime->fs == NULL || config == NULL ||
        config->app_generation == NULL || config->app_generation[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_pal_fs_open(
        runtime->fs,
        H2_PARTIAL_UPDATE_DATA_PATH,
        H2_PAL_FS_OPEN_READ,
        &file);
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=data_open app=%s rc=%d\n",
            config->app_generation, rc);
        return rc;
    }
    rc = h2_pal_fs_read(
        runtime->fs, file, data_generation, sizeof(data_generation) - 1u, &read_len);
    {
        int close_rc = h2_pal_fs_close(runtime->fs, file);
        if (rc == H2_PAL_OK) {
            rc = close_rc;
        }
    }
    if (rc != H2_PAL_OK || read_len == 0u) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=data_read app=%s rc=%d\n",
            config->app_generation, rc);
        return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
    }
    while (read_len > 0u &&
        (data_generation[read_len - 1u] == '\n' ||
            data_generation[read_len - 1u] == '\r')) {
        read_len -= 1u;
    }
    data_generation[read_len] = '\0';
    if (read_len == 0u || strchr(data_generation, ' ') != NULL) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=data_format app=%s\n",
            config->app_generation);
        return H2_PAL_ERR_FORMAT;
    }
    printf("H2_PARTIAL_UPDATE_SMOKE result=PASS app=%s data=%s\n",
        config->app_generation, data_generation);
    return H2_PAL_OK;
}
