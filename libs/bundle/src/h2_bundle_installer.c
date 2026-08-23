#include "h2_bundle_installer.h"

#include "h2_bundle_archive.h"

#include <stdlib.h>
#include <string.h>

static void *fallback_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *fallback_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void fallback_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static h2_pal_mem_api_t *fallback_allocator(void) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = fallback_alloc,
        .realloc = fallback_realloc,
        .free = fallback_free,
    };
    static h2_pal_mem_api_t allocator = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &allocator;
}

static int bundle_fs_open(const h2_pal_fs_api_t *fs, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file) {
    if (fs == NULL || path == NULL || out_file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    return h2_pal_fs_open(fs, path, mode, out_file);
}

static int bundle_fs_read(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file, void *data, size_t len, size_t *out_read) {
    if (fs == NULL || file == NULL || data == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_read(fs, file, data, len, out_read);
}

static int bundle_fs_write_all(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file, const void *data, size_t len) {
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = len;

    if (fs == NULL || file == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    while (remaining > 0u) {
        size_t written = 0u;
        int rc = h2_pal_fs_write(fs, file, cursor, remaining, &written);
        if (rc != H2_PAL_FS_OK) {
            return rc;
        }
        if (written == 0u || written > remaining) {
            return H2_PAL_FS_ERR_IO;
        }
        cursor += written;
        remaining -= written;
    }
    return H2_PAL_FS_OK;
}

static int bundle_fs_sync(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file) {
    if (fs == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_sync(fs, file);
}

static int bundle_fs_close(const h2_pal_fs_api_t *fs, h2_pal_fs_file_t *file) {
    if (fs == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return h2_pal_fs_close(fs, file);
}

static int bundle_fs_write_file(const h2_pal_fs_api_t *fs, const char *path, const void *data, size_t len) {
    h2_pal_fs_file_t *file = NULL;
    int rc = bundle_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    int close_rc;

    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = bundle_fs_write_all(fs, file, data, len);
    if (rc == H2_PAL_FS_OK) {
        rc = bundle_fs_sync(fs, file);
    }
    close_rc = bundle_fs_close(fs, file);
    return rc == H2_PAL_FS_OK ? close_rc : rc;
}

int h2_bundle_installer_init(h2_bundle_installer_t *installer, const h2_pal_fs_api_t *fs, const h2_pal_mem_api_t *allocator) {
    if (installer == NULL || fs == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    memset(installer, 0, sizeof(*installer));
    installer->fs = fs;
    installer->allocator = allocator == NULL ? fallback_allocator() : allocator;
    return H2_BUNDLE_OK;
}

void h2_bundle_installer_set_progress(h2_bundle_installer_t *installer, h2_bundle_progress_fn progress, void *user) {
    if (installer == NULL) {
        return;
    }
    installer->progress = progress;
    installer->progress_user = user;
}

int h2_bundle_install(h2_bundle_installer_t *installer, const h2_bundle_install_options_t *options) {
    int rc;

    if (installer == NULL || options == NULL || options->archive_path == NULL || options->dst_root == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (options->manifest != NULL && options->manifest->entry_count > 0u && options->manifest->entries == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }

    rc = h2_bundle_archive_install_zlib_tar(installer, options);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (options->installed_version_path != NULL && options->manifest != NULL && options->manifest->version != NULL) {
        size_t len = strlen(options->manifest->version);
        rc = bundle_fs_write_file(installer->fs, options->installed_version_path, options->manifest->version, len);
        if (rc != H2_PAL_FS_OK) {
            return H2_BUNDLE_ERR_FS;
        }
    }
    return H2_BUNDLE_OK;
}

int h2_bundle_install_archive(h2_bundle_installer_t *installer, const char *archive_path, const char *dst_root) {
    h2_bundle_install_options_t options;

    if (installer == NULL || archive_path == NULL || dst_root == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    memset(&options, 0, sizeof(options));
    options.archive_path = archive_path;
    options.dst_root = dst_root;
    return h2_bundle_install(installer, &options);
}

int h2_bundle_install_ota(h2_bundle_installer_t *installer, const h2_bundle_ota_options_t *options) {
    h2_bundle_install_options_t install_options;

    if (installer == NULL || options == NULL || options->archive_path == NULL ||
        options->data_root == NULL ||
        (!options->skip_data_install && options->installed_checksum_path == NULL)) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    memset(&install_options, 0, sizeof(install_options));
    install_options.archive_path = options->archive_path;
    install_options.dst_root = options->data_root;
    install_options.ota_layout = 1;
    install_options.installed_checksum_path = options->installed_checksum_path;
    install_options.app_writer = options->app_writer;
    install_options.clear_data = options->clear_data;
    install_options.clear_data_user = options->clear_data_user;
    install_options.skip_app_install = options->skip_app_install;
    install_options.skip_data_install = options->skip_data_install;
    return h2_bundle_install(installer, &install_options);
}

int h2_bundle_installed_version_matches(const h2_pal_fs_api_t *fs, const char *installed_version_path, const char *version, int *out_matches) {
    h2_pal_fs_file_t *file = NULL;
    char buf[64];
    size_t read_len = 0u;
    int rc;

    if (fs == NULL || installed_version_path == NULL || version == NULL || out_matches == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    *out_matches = 0;
    rc = bundle_fs_open(fs, installed_version_path, H2_PAL_FS_OPEN_READ, &file);
    if (rc == H2_PAL_FS_ERR_NOT_FOUND) {
        return H2_BUNDLE_OK;
    }
    if (rc != H2_PAL_FS_OK) {
        return H2_BUNDLE_ERR_FS;
    }
    rc = bundle_fs_read(fs, file, buf, sizeof(buf) - 1u, &read_len);
    if (rc == H2_PAL_FS_OK) {
        buf[read_len] = '\0';
        *out_matches = strcmp(buf, version) == 0 ? 1 : 0;
    }
    bundle_fs_close(fs, file);
    return rc == H2_PAL_FS_OK ? H2_BUNDLE_OK : H2_BUNDLE_ERR_FS;
}
