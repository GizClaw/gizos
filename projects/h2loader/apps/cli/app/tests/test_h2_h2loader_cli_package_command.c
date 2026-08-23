#include "h2_h2loader_cli_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory_file {
    uint8_t data[4096];
    size_t len;
} memory_file_t;

typedef struct memory_fs {
    memory_file_t published;
    int fail_write;
} memory_fs_t;

static int fs_open(
    void *user, const char *path, h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    memory_fs_t *fs = user;
    if (strcmp(path, "/tmp/golden") != 0 ||
        mode != H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(&fs->published, 0, sizeof(fs->published));
    *out_file = (h2_pal_fs_file_t *)&fs->published;
    return H2_PAL_OK;
}

static int fs_write(
    void *user, h2_pal_fs_file_t *opaque, const void *data, size_t len,
    size_t *out_written) {
    memory_file_t *file = (memory_file_t *)opaque;
    memory_fs_t *fs = user;
    if (fs->fail_write) return H2_PAL_ERR_IO;
    if (len > sizeof(file->data) - file->len) return H2_PAL_ERR_NO_SPACE;
    memcpy(&file->data[file->len], data, len);
    file->len += len;
    *out_written = len;
    return H2_PAL_OK;
}

static int fs_sync(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    (void)file;
    return H2_PAL_OK;
}

static int fs_close(void *user, h2_pal_fs_file_t *file) {
    (void)user;
    (void)file;
    return H2_PAL_OK;
}

static h2_pal_result_t package_read(
    void *user, uint64_t offset, uint8_t *out, size_t out_size,
    size_t *out_read) {
    memory_file_t *file = user;
    if (offset > file->len) return H2_PAL_ERR_INVALID_ARG;
    *out_read = file->len - (size_t)offset < out_size
        ? file->len - (size_t)offset : out_size;
    memcpy(out, &file->data[offset], *out_read);
    return H2_PAL_OK;
}

static void *mem_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *mem_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void mem_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static h2_pal_result_t time_monotonic_ms(void *user, uint64_t *out_ms) {
    (void)user;
    *out_ms = 42u;
    return H2_PAL_OK;
}

static h2_pal_result_t discard_write(
    void *user, const void *data, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
    (void)user;
    (void)data;
    (void)timeout_ms;
    *out_written = len;
    return H2_PAL_OK;
}

int main(void) {
    static const h2_pal_fs_vtable_t fs_vtable = {
        .open = fs_open,
        .write = fs_write,
        .sync = fs_sync,
        .close = fs_close,
    };
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = mem_alloc,
        .realloc = mem_realloc,
        .free = mem_free,
    };
    static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = time_monotonic_ms,
    };
    static const h2_pal_time_api_t time = {.vtable = &time_vtable};
    static const h2_command_io_vtable_t io_vtable = {.write = discard_write};
    memory_fs_t fs_state = {0};
    h2_pal_fs_api_t fs = {.user = &fs_state, .vtable = &fs_vtable};
    h2_command_io_api_t io = {.vtable = &io_vtable};
    h2_runtime_t runtime = {.mem = &mem, .time = &time, .fs = &fs};
    h2_h2loader_cli_config_t config = {
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {.runtime = &runtime, .config = &config};
    const char *argv[] = {"--out", "/tmp/golden"};

    assert(h2_h2loader_cli_package_command(&context, 2, argv, 1) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(fs_state.published.len == 358u);
    h2_h2loader_host_catalog_entry_t asset;
    const h2_h2loader_host_package_inspect_config_t inspect = {
        .allocator = &mem,
        .read_payload = package_read,
        .payload_user = &fs_state.published,
        .payload_bytes = fs_state.published.len,
    };
    assert(h2_h2loader_host_package_inspect(&inspect, &asset) == H2_PAL_OK);
    assert(strcmp(asset.board, "fixture") == 0);
    assert(strcmp(asset.target, "host") == 0);

    {
        const char *relative[] = {"--out", "golden"};
        assert(h2_h2loader_cli_package_command(&context, 2, relative, 1) ==
            H2_H2LOADER_CLI_EXIT_RUNTIME);
    }
    {
        const char *data_dir[] = {
            "--app-bin", "/tmp/app.bin",
            "--data-dir", "/tmp/data",
            "--board", "fixture",
            "--target", "host",
            "--version", "0",
        };
        assert(h2_h2loader_cli_package_command(
            &context, 10, data_dir, 0) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    }
    fs_state.fail_write = 1;
    assert(h2_h2loader_cli_package_command(&context, 2, argv, 1) ==
        H2_H2LOADER_CLI_EXIT_RUNTIME);
    return 0;
}
