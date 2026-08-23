#include "h2_h2loader_cli_internal.h"

#include <string.h>

typedef struct file_source {
    const h2_pal_fs_api_t *fs;
    h2_pal_fs_file_t *file;
} file_source_t;

typedef struct memory_source {
    const uint8_t *data;
    size_t len;
} memory_source_t;

static h2_pal_result_t source_read(
    void *user, uint64_t offset, uint8_t *out, size_t out_size,
    size_t *out_read) {
    file_source_t *source = user;
    if (source == NULL || source->fs == NULL || source->file == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = h2_pal_fs_seek(source->fs, source->file, offset);
    return result == H2_PAL_OK
        ? h2_pal_fs_read(source->fs, source->file, out, out_size, out_read)
        : result;
}

static h2_pal_result_t memory_read(
    void *user, uint64_t offset, uint8_t *out, size_t out_size,
    size_t *out_read) {
    const memory_source_t *source = user;
    if (source == NULL || out_read == NULL || offset > source->len ||
        (out == NULL && out_size != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = source->len - (size_t)offset < out_size
        ? source->len - (size_t)offset : out_size;
    if (*out_read != 0u) memcpy(out, &source->data[offset], *out_read);
    return H2_PAL_OK;
}

static h2_pal_result_t package_write(void *user, const uint8_t *data, size_t len) {
    file_source_t *output = user;
    if (output == NULL || output->fs == NULL || output->file == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (len != 0u) {
        size_t written = 0u;
        h2_pal_result_t result = h2_pal_fs_write(
            output->fs, output->file, data, len, &written);
        if (result != H2_PAL_OK) return result;
        if (written == 0u || written > len) return H2_PAL_ERR_IO;
        data += written;
        len -= written;
    }
    return H2_PAL_OK;
}

static const char *option_value(
    int argc, const char *const *argv, int *index, const char *name) {
    if (strcmp(argv[*index], name) != 0 || *index + 1 >= argc) return NULL;
    ++*index;
    return argv[*index];
}

int h2_h2loader_cli_package_command(
    h2_h2loader_cli_context_t *context, int argc, const char *const *argv,
    int golden) {
    const char *out = "/tmp/update.tar.zlib";
    const char *app_bin = NULL;
    const char *app_path = "app/esp/app.bin";
    const char *data_dir = NULL;
    const char *role = "app";
    const char *board = golden ? "fixture" : NULL;
    const char *target = golden ? "host" : NULL;
    const char *version = golden ? "0" : NULL;
    static const uint8_t golden_app[] = {0xe9u, 1u, 2u, 3u, 4u, 5u};
    static const uint8_t golden_alpha[] = "alpha";
    static const uint8_t golden_zed[] = {0u, 1u, 2u};
    memory_source_t golden_app_source = {
        .data = golden_app, .len = sizeof(golden_app),
    };
    memory_source_t golden_alpha_source = {
        .data = golden_alpha, .len = sizeof(golden_alpha) - 1u,
    };
    memory_source_t golden_zed_source = {
        .data = golden_zed, .len = sizeof(golden_zed),
    };
    h2_h2loader_host_package_source_t golden_data[] = {
        {
            .name = "data/z.bin", .size = sizeof(golden_zed),
            .read = memory_read, .user = &golden_zed_source,
        },
        {
            .name = "data/a.txt", .size = sizeof(golden_alpha) - 1u,
            .read = memory_read, .user = &golden_alpha_source,
        },
    };
    file_source_t app_file = {0};
    file_source_t output = {0};
    h2_h2loader_host_package_writer_config_t writer = {0};
    h2_h2loader_host_package_writer_result_t result;
    h2_pal_result_t rc = H2_PAL_OK;

    if (context == NULL || context->runtime == NULL ||
        context->runtime->mem == NULL || context->runtime->fs == NULL ||
        (argc != 0 && argv == NULL)) {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    app_file.fs = context->runtime->fs;
    output.fs = context->runtime->fs;

    for (int i = 0; i < argc; ++i) {
        const char *value = NULL;
        if ((value = option_value(argc, argv, &i, "--out")) != NULL) out = value;
        else if ((value = option_value(argc, argv, &i, "--app-bin")) != NULL) app_bin = value;
        else if ((value = option_value(argc, argv, &i, "--app-path")) != NULL) app_path = value;
        else if (!golden && (value = option_value(argc, argv, &i, "--data-dir")) != NULL) data_dir = value;
        else if (!golden && (value = option_value(argc, argv, &i, "--role")) != NULL) role = value;
        else if (!golden && (value = option_value(argc, argv, &i, "--board")) != NULL) board = value;
        else if (!golden && (value = option_value(argc, argv, &i, "--target")) != NULL) target = value;
        else if (!golden && (value = option_value(argc, argv, &i, "--version")) != NULL) version = value;
        else {
            h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
                "h2loader: invalid package option: %s\n", argv[i]);
            return H2_H2LOADER_CLI_EXIT_USAGE;
        }
    }
    if ((!golden && app_bin == NULL) || board == NULL || target == NULL ||
        version == NULL || (strcmp(role, "app") != 0 &&
                            strcmp(role, "h2loader") != 0)) {
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    if (data_dir != NULL) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: --data-dir is unsupported because PAL FS has no directory enumeration\n");
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    if (out[0] != '/' || (app_bin != NULL && app_bin[0] != '/') ||
        app_path == NULL || app_path[0] == '\0') {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    if (app_bin != NULL) {
        h2_pal_fs_stat_t stat_value;
        rc = h2_pal_fs_stat(context->runtime->fs, app_bin, &stat_value);
        if (rc == H2_PAL_OK && stat_value.is_dir) rc = H2_PAL_ERR_INVALID_ARG;
        if (rc == H2_PAL_OK) {
            rc = h2_pal_fs_open(context->runtime->fs, app_bin,
                H2_PAL_FS_OPEN_READ, &app_file.file);
        }
        if (rc != H2_PAL_OK) goto cleanup;
        writer.app = (h2_h2loader_host_package_source_t){
            .name = app_path, .size = stat_value.size,
            .read = source_read, .user = &app_file,
        };
    } else {
        writer.app = (h2_h2loader_host_package_source_t){
            .name = app_path, .size = sizeof(golden_app),
            .read = memory_read, .user = &golden_app_source,
        };
        writer.data_entries = golden_data;
        writer.data_entry_count = sizeof(golden_data) / sizeof(golden_data[0]);
    }
    rc = h2_pal_fs_open(context->runtime->fs, out,
        H2_PAL_FS_OPEN_WRITE_TRUNCATE, &output.file);
    if (rc != H2_PAL_OK) goto cleanup;
    writer.allocator = context->runtime->mem;
    writer.role = role;
    writer.board = board;
    writer.target = target;
    writer.version = version;
    writer.write = package_write;
    writer.write_user = &output;
    rc = h2_h2loader_host_package_write(&writer, &result);
    if (rc == H2_PAL_OK) rc = h2_pal_fs_sync(context->runtime->fs, output.file);
    if (output.file != NULL) {
        h2_pal_result_t close_result = h2_pal_fs_close(
            context->runtime->fs, output.file);
        output.file = NULL;
        if (rc == H2_PAL_OK) rc = close_result;
    }

cleanup:
    if (output.file != NULL) {
        (void)h2_pal_fs_close(context->runtime->fs, output.file);
    }
    if (app_file.file != NULL) {
        (void)h2_pal_fs_close(context->runtime->fs, app_file.file);
    }
    if (rc != H2_PAL_OK) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: package failed code=%d\n", rc);
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_LOADER_PACKAGE result=OK path=%s bytes=%llu image_sha256=%s checksum=%s\n",
        out, (unsigned long long)result.package_bytes,
        result.image_sha256, result.data_sha256);
    return H2_H2LOADER_CLI_EXIT_OK;
}
