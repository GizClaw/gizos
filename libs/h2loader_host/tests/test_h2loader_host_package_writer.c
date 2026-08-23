#include "h2_h2loader_host_package.h"
#include "h2_h2loader_host_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes {
    const uint8_t *data;
    size_t len;
} bytes_t;

typedef struct output {
    uint8_t data[65536];
    size_t len;
    h2_pal_result_t result;
} output_t;

static void *mem_alloc(void *user, size_t len) { (void)user; return malloc(len); }
static void *mem_realloc(void *user, void *ptr, size_t len) { (void)user; return realloc(ptr, len); }
static void mem_free(void *user, void *ptr) { (void)user; free(ptr); }

static h2_pal_result_t read_bytes(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    bytes_t *bytes = user;
    if (offset > bytes->len || out_read == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_read = bytes->len - (size_t)offset < out_size
        ? bytes->len - (size_t)offset : out_size;
    memcpy(out, &bytes->data[offset], *out_read);
    return H2_PAL_OK;
}

static h2_pal_result_t write_bytes(void *user, const uint8_t *data, size_t len) {
    output_t *output = user;
    if (output->result != H2_PAL_OK) return output->result;
    if (len > sizeof(output->data) - output->len) return H2_PAL_ERR_NO_SPACE;
    memcpy(&output->data[output->len], data, len);
    output->len += len;
    return H2_PAL_OK;
}

int main(void) {
    static const uint8_t app_data[] = {0xe9u, 1u, 2u, 3u, 4u, 5u};
    static const uint8_t alpha[] = "alpha";
    static const uint8_t zed[] = {0u, 1u, 2u};
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = mem_alloc, .realloc = mem_realloc, .free = mem_free,
    };
    static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
    bytes_t app = {.data = app_data, .len = sizeof(app_data)};
    bytes_t first = {.data = zed, .len = sizeof(zed)};
    bytes_t second = {.data = alpha, .len = sizeof(alpha) - 1u};
    h2_h2loader_host_package_source_t data[] = {
        {.name = "data/z.bin", .size = sizeof(zed), .read = read_bytes, .user = &first},
        {.name = "data/a.txt", .size = sizeof(alpha) - 1u, .read = read_bytes, .user = &second},
    };
    output_t output = {0};
    h2_h2loader_host_package_writer_config_t config = {
        .allocator = &mem,
        .role = "app",
        .board = "fixture",
        .target = "host",
        .version = "0",
        .app = {.name = "app/esp/app.bin", .size = sizeof(app_data), .read = read_bytes, .user = &app},
        .data_entries = data,
        .data_entry_count = sizeof(data) / sizeof(data[0]),
        .write = write_bytes,
        .write_user = &output,
    };
    h2_h2loader_host_package_writer_result_t result;
    assert(h2_h2loader_host_package_write(&config, &result) == H2_PAL_OK);
    assert(output.len == 358u);
    h2_h2loader_host_sha256_t sha;
    uint8_t digest[32];
    char hex[65];
    h2_h2loader_host_sha256_init(&sha);
    h2_h2loader_host_sha256_update(&sha, output.data, output.len);
    h2_h2loader_host_sha256_finish(&sha, digest);
    h2_h2loader_host_sha256_hex(digest, hex);
    assert(strcmp(hex, "ec3b1ea6de8de1e1862b801d0c2a00555f294be2eb9c3ec58a92ef707837790b") == 0);

    bytes_t package = {.data = output.data, .len = output.len};
    h2_h2loader_host_package_inspect_config_t inspect = {
        .allocator = &mem,
        .read_payload = read_bytes,
        .payload_user = &package,
        .payload_bytes = output.len,
    };
    h2_h2loader_host_catalog_entry_t asset;
    assert(h2_h2loader_host_package_inspect(&inspect, &asset) == H2_PAL_OK);
    assert(strcmp(asset.board, "fixture") == 0);
    assert(strcmp(asset.target, "host") == 0);
    assert(asset.bytes == output.len);

    data[1].name = "data/z.bin";
    assert(h2_h2loader_host_package_write(&config, &result) == H2_PAL_ERR_INVALID_ARG);
    data[1].name = "data/a.txt";
    data[1].name = "data/..";
    assert(h2_h2loader_host_package_write(&config, &result) == H2_PAL_ERR_INVALID_ARG);
    data[1].name = "data/a.txt";
    config.data_entry_count = SIZE_MAX;
    assert(h2_h2loader_host_package_write(&config, &result) == H2_PAL_ERR_INVALID_ARG);
    config.data_entry_count = sizeof(data) / sizeof(data[0]);
    output.result = H2_PAL_ERR_IO;
    output.len = 0u;
    assert(h2_h2loader_host_package_write(&config, &result) == H2_PAL_ERR_IO);
    return 0;
}
