#include "h2_bundle.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_fs_file {
    size_t offset;
    int output_kind;
};

typedef struct bundle_test_context {
    const uint8_t *archive;
    size_t archive_len;
    struct h2_pal_fs_file file;
    struct h2_pal_fs_file output_file;
    int app_begins;
    int app_writes;
    int app_ends;
    int app_aborts;
    int data_clears;
    int data_writes;
    int checksum_writes;
} bundle_test_context_t;

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static int test_fs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    if (mode == H2_PAL_FS_OPEN_READ) {
        if (strcmp(path, "/data/.checksum") == 0) {
            return H2_PAL_FS_ERR_NOT_FOUND;
        }
        assert(strcmp(path, "/dl/update.tar.zlib") == 0);
        context->file.output_kind = 0;
        context->file.offset = 0u;
        *out_file = &context->file;
    } else {
        assert(mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE);
        assert(strcmp(path, "/data/version.txt") == 0 ||
            strcmp(path, "/data/.checksum") == 0);
        context->output_file.output_kind =
            strcmp(path, "/data/.checksum") == 0 ? 2 : 1;
        context->output_file.offset = 0u;
        *out_file = &context->output_file;
    }
    return H2_PAL_FS_OK;
}

static int test_fs_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    assert(file->output_kind == 0);
    size_t remaining = context->archive_len - file->offset;
    size_t take = remaining < len ? remaining : len;
    memcpy(data, context->archive + file->offset, take);
    file->offset += take;
    *out_read = take;
    return H2_PAL_FS_OK;
}

static int test_fs_write(
    void *user,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    (void)data;
    if (file->output_kind == 1) {
        context->data_writes += 1;
    } else {
        assert(file->output_kind == 2);
        context->checksum_writes += 1;
    }
    *out_written = len;
    return H2_PAL_FS_OK;
}

static int test_fs_sync(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    assert(file->output_kind != 0);
    return H2_PAL_FS_OK;
}

static int test_fs_mkdir(void *user, const char *path) {
    (void)user;
    assert(strncmp(path, "/data", strlen("/data")) == 0);
    return H2_PAL_FS_OK;
}

static int test_fs_close(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    (void)file;
    return H2_PAL_FS_OK;
}

static int app_begin(void *user, const h2_bundle_entry_t *entry) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    (void)entry;
    context->app_begins += 1;
    return H2_BUNDLE_OK;
}

static int app_write(
    void *user,
    const h2_bundle_entry_t *entry,
    const void *data,
    size_t len) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    (void)entry;
    (void)data;
    (void)len;
    context->app_writes += 1;
    return H2_BUNDLE_OK;
}

static int app_end(void *user, const h2_bundle_entry_t *entry) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    (void)entry;
    context->app_ends += 1;
    return H2_BUNDLE_OK;
}

static void app_abort(void *user) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    context->app_aborts += 1;
}

static int clear_data(void *user, const char *data_root) {
    bundle_test_context_t *context = (bundle_test_context_t *)user;
    assert(strcmp(data_root, "/data") == 0);
    context->data_clears += 1;
    return H2_BUNDLE_OK;
}

static size_t append_tar_file(
    uint8_t *tar,
    size_t capacity,
    size_t offset,
    const char *path,
    const void *data,
    size_t len) {
    uint8_t *header;
    uint64_t sum = 0u;
    size_t padded_len = (len + 511u) & ~(size_t)511u;

    assert(strlen(path) < 100u);
    assert(offset <= capacity && 512u + padded_len <= capacity - offset);
    header = tar + offset;
    memset(header, 0, 512u + padded_len);
    memcpy(header, path, strlen(path));
    (void)snprintf((char *)header + 124u, 12u, "%011llo",
        (unsigned long long)len);
    memset(header + 148u, ' ', 8u);
    header[156u] = '0';
    for (size_t i = 0u; i < 512u; ++i) {
        sum += header[i];
    }
    (void)snprintf((char *)header + 148u, 8u, "%06llo",
        (unsigned long long)sum);
    header[154u] = '\0';
    header[155u] = ' ';
    memcpy(header + 512u, data, len);
    return offset + 512u + padded_len;
}

static size_t wrap_zlib(
    uint8_t *out,
    size_t capacity,
    const uint8_t *data,
    size_t len) {
    uint32_t sum1 = 1u;
    uint32_t sum2 = 0u;
    uint16_t block_len;
    uint16_t inverse_len;

    assert(len <= UINT16_MAX && capacity >= len + 11u);
    block_len = (uint16_t)len;
    inverse_len = (uint16_t)~block_len;
    out[0] = 0x78u;
    out[1] = 0x01u;
    out[2] = 0x01u;
    out[3] = (uint8_t)block_len;
    out[4] = (uint8_t)(block_len >> 8u);
    out[5] = (uint8_t)inverse_len;
    out[6] = (uint8_t)(inverse_len >> 8u);
    memcpy(out + 7u, data, len);
    for (size_t i = 0u; i < len; ++i) {
        sum1 = (sum1 + data[i]) % 65521u;
        sum2 = (sum2 + sum1) % 65521u;
    }
    out[len + 7u] = (uint8_t)(sum2 >> 8u);
    out[len + 8u] = (uint8_t)sum2;
    out[len + 9u] = (uint8_t)(sum1 >> 8u);
    out[len + 10u] = (uint8_t)sum1;
    return len + 11u;
}

static size_t make_archive(uint8_t *out, size_t capacity, size_t app_count) {
    static const char manifest[] = "format=1\n";
    static const char checksum[] =
        "abababababababababababababababababababababababababababababababab\n";
    static const char data[] = "v1\n";
    static const char app[] = "image";
    uint8_t tar[8192];
    size_t tar_len = 0u;

    memset(tar, 0, sizeof(tar));
    tar_len = append_tar_file(tar, sizeof(tar), tar_len,
        "manifest", manifest, sizeof(manifest) - 1u);
    tar_len = append_tar_file(tar, sizeof(tar), tar_len,
        "checksum", checksum, sizeof(checksum) - 1u);
    tar_len = append_tar_file(tar, sizeof(tar), tar_len,
        "data/version.txt", data, sizeof(data) - 1u);
    for (size_t i = 0u; i < app_count; ++i) {
        tar_len = append_tar_file(tar, sizeof(tar), tar_len,
            "app/esp/app.bin", app, sizeof(app) - 1u);
    }
    tar_len += 1024u;
    return wrap_zlib(out, capacity, tar, tar_len);
}

static int install_archive(
    bundle_test_context_t *context,
    int skip_app,
    int skip_data) {
    h2_bundle_installer_t installer;
    h2_bundle_ota_options_t options = {0};
    const h2_pal_fs_vtable_t fs_vtable = {
        .mkdir = test_fs_mkdir,
        .open = test_fs_open,
        .read = test_fs_read,
        .write = test_fs_write,
        .sync = test_fs_sync,
        .close = test_fs_close,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    const h2_pal_fs_api_t fs = {.user = context, .vtable = &fs_vtable};
    const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    const h2_bundle_app_writer_t writer = {
        .user = context,
        .begin = app_begin,
        .write = app_write,
        .end = app_end,
        .abort = app_abort,
    };

    assert(h2_bundle_installer_init(&installer, &fs, &mem) == H2_BUNDLE_OK);
    options.archive_path = "/dl/update.tar.zlib";
    options.data_root = "/data";
    options.installed_checksum_path = "/data/.checksum";
    options.app_writer = &writer;
    options.clear_data = clear_data;
    options.clear_data_user = context;
    options.skip_app_install = skip_app;
    options.skip_data_install = skip_data;
    return h2_bundle_install_ota(&installer, &options);
}

int main(void) {
    uint8_t archive[8203];
    bundle_test_context_t context = {0};

    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 1u);
    assert(install_archive(&context, 1, 1) == H2_BUNDLE_OK);
    assert(context.app_begins == 0);
    assert(context.app_writes == 0);
    assert(context.app_ends == 0);
    assert(context.app_aborts == 0);
    assert(context.data_clears == 0);
    assert(context.data_writes == 0);
    assert(context.checksum_writes == 0);

    memset(&context, 0, sizeof(context));
    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 1u);
    assert(install_archive(&context, 0, 1) == H2_BUNDLE_OK);
    assert(context.app_begins == 1);
    assert(context.app_writes > 0);
    assert(context.app_ends == 1);
    assert(context.data_clears == 0);
    assert(context.data_writes == 0);
    assert(context.checksum_writes == 0);

    memset(&context, 0, sizeof(context));
    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 1u);
    assert(install_archive(&context, 1, 0) == H2_BUNDLE_OK);
    assert(context.app_begins == 0);
    assert(context.app_writes == 0);
    assert(context.app_ends == 0);
    assert(context.data_clears == 1);
    assert(context.data_writes > 0);
    assert(context.checksum_writes > 0);

    memset(&context, 0, sizeof(context));
    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 1u);
    assert(install_archive(&context, 0, 0) == H2_BUNDLE_OK);
    assert(context.app_begins == 1);
    assert(context.app_writes > 0);
    assert(context.app_ends == 1);
    assert(context.data_clears == 1);
    assert(context.data_writes > 0);
    assert(context.checksum_writes > 0);

    memset(&context, 0, sizeof(context));
    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 0u);
    assert(install_archive(&context, 1, 1) == H2_BUNDLE_ERR_LAYOUT);

    memset(&context, 0, sizeof(context));
    context.archive = archive;
    context.archive_len = make_archive(archive, sizeof(archive), 2u);
    assert(install_archive(&context, 1, 1) == H2_BUNDLE_ERR_LAYOUT);
    return 0;
}
