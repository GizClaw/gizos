#include "h2_bundle_segmented.h"
#include "h2_bundle_archive.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

typedef struct bytes { const uint8_t *data; size_t size, emitted, chunk; int fail; } bytes_t;
static void *allocate(void *user, size_t len) { (void)user; return malloc(len); }
static void release(void *user, void *ptr) { (void)user; free(ptr); }
static int read_bytes(void *user, uint64_t offset, uint8_t *out, size_t cap, size_t *got) {
    bytes_t *b = user;
    if (offset > b->size) return H2_PAL_ERR_INVALID_ARG;
    *got = b->size - (size_t)offset < cap ? b->size - (size_t)offset : cap;
    if (b->chunk != 0u && *got > b->chunk) *got = b->chunk;
    memcpy(out, b->data + offset, *got); return H2_PAL_OK;
}
static int write_bytes(void *user, const uint8_t *data, size_t len) {
    bytes_t *b = user;
    if (b->fail) return H2_PAL_ERR_IO;
    assert(len <= b->size - b->emitted);
    assert(memcmp(data, b->data + b->emitted, len) == 0);
    b->emitted += len; return H2_PAL_OK;
}

static int nop_start(void *user) { (void)user; return H2_PAL_OK; }
static int nop_update(void *user, const uint8_t *data, size_t len) {
    (void)user; (void)data; (void)len; return H2_PAL_OK;
}
static int empty_finish(void *user, uint8_t hash[32]) {
    (void)user;
    static const uint8_t empty[32] = {0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,
        0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    memcpy(hash, empty, sizeof(empty)); return H2_PAL_OK;
}
static void nop_abort(void *user) { (void)user; }
static int no_write(void *user, h2_pal_fs_file_t *file, const void *data, size_t len, size_t *written) {
    (void)user; (void)file; (void)data; (void)len; (void)written; assert(0); return H2_PAL_ERR_IO;
}
static int no_open(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out) {
    (void)user; (void)path; (void)mode; (void)out; assert(0); return H2_PAL_ERR_IO;
}
static int remove_checksum(void *user, const char *path) { (void)user; (void)path; return H2_PAL_OK; }
static int clear_tree(void *user, const char *path) { (void)user; (void)path; return H2_PAL_OK; }

static void test_data_only_envelope(const h2_pal_mem_api_t *mem) {
    uint8_t tar[2048] = {0}, compressed[2048];
    const h2_pal_fs_vtable_t fv = {.open = no_open, .write = no_write, .remove = remove_checksum};
    const h2_pal_fs_api_t fs = {.vtable = &fv};
    h2_bundle_installer_t installer;
    assert(h2_bundle_installer_init(&installer, &fs, mem) == H2_PAL_OK);
    const h2_bundle_ota_options_t options = {.data_root = "/data", .installed_checksum_path = "/data/.checksum",
        .clear_data = clear_tree};
    const h2_bundle_digest_api_t digest = {.start = nop_start, .update = nop_update,
        .finish = empty_finish, .abort = nop_abort};
    h2_bundle_segmented_manifest_t m = {0};
    memcpy(m.data_sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 65u);
    /* One zero block must not commit an empty data tree. */
    uLongf len = sizeof(compressed);
    assert(compress2(compressed, &len, tar, 512u, 6) == Z_OK);
    bytes_t input = {.data = compressed, .size = len};
    m.data = (h2_bundle_segment_t){.compressed_size = len, .size = 512u};
    assert(h2_bundle_archive_install_data_zlib(&installer, &options, read_bytes, &input, &m, &digest) != H2_PAL_OK);
    /* Legacy OTA accepts app directory headers; the data-only path must reject them. */
    memcpy(tar, "app", 3u); tar[156] = '5';
    memset(tar + 148u, ' ', 8u);
    unsigned sum = 0u;
    for (size_t i = 0u; i < 512u; ++i) sum += tar[i];
    (void)snprintf((char *)tar + 148u, 8u, "%06o", sum);
    tar[154] = '\0'; tar[155] = ' ';
    len = sizeof(compressed);
    assert(compress2(compressed, &len, tar, sizeof(tar), 6) == Z_OK);
    input.size = len; m.data.compressed_size = len; m.data.size = sizeof(tar);
    assert(h2_bundle_archive_install_data_zlib(&installer, &options, read_bytes, &input, &m, &digest) != H2_PAL_OK);
}

static void test_incompressible_members(const h2_pal_mem_api_t *mem) {
    uint8_t raw[65536], compressed[70000];
    uint32_t state = 0x12345678u;
    for (size_t i = 0u; i < sizeof(raw); ++i) {
        state ^= state << 13u; state ^= state >> 17u; state ^= state << 5u;
        raw[i] = (uint8_t)state;
    }
    const int levels[] = {0, 6};
    for (size_t i = 0u; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        uLongf len = sizeof(compressed);
        assert(compress2(compressed, &len, raw, sizeof(raw), levels[i]) == Z_OK);
        bytes_t source = {.data = compressed, .size = len};
        bytes_t output = {.data = raw, .size = sizeof(raw)};
        const h2_bundle_segment_t segment = {.compressed_size = len, .size = sizeof(raw)};
        assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, mem,
            write_bytes, &output) == H2_PAL_OK);
        assert(output.emitted == sizeof(raw));
    }
}
int main(void) {
    uint8_t raw[20000], compressed[21000];
    for (size_t i = 0u; i < sizeof(raw); ++i) raw[i] = (uint8_t)(i % 251u);
    uLongf size = sizeof(compressed) - 1u;
    assert(compress2(compressed, &size, raw, sizeof(raw), 6) == Z_OK);
    const h2_pal_mem_vtable_t mv = {.alloc = allocate, .free = release};
    const h2_pal_mem_api_t mem = {.vtable = &mv};
    bytes_t source = {.data = compressed, .size = size, .chunk = 1u};
    bytes_t output = {.data = raw, .size = sizeof(raw)};
    h2_bundle_segment_t segment = {.compressed_size = size, .size = sizeof(raw)};
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) == H2_PAL_OK);
    assert(output.emitted == sizeof(raw));
    output.emitted = 0u; --segment.size;
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) == H2_PAL_ERR_FORMAT);
    output.emitted = 0u; segment.size += 2u;
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) == H2_PAL_ERR_TRUNCATED);
    segment.size = sizeof(raw); output.emitted = 0u;
    --segment.compressed_size;
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) != H2_PAL_OK);
    segment.compressed_size += 2u; ++source.size; compressed[size] = 0u; output.emitted = 0u;
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) == H2_PAL_ERR_FORMAT);
    --segment.compressed_size; --source.size; output.emitted = 0u; output.fail = 1;
    assert(h2_bundle_segmented_inflate(read_bytes, &source, &segment, &mem, write_bytes, &output) == H2_PAL_ERR_IO);
    h2_bundle_segmented_manifest_t m;
    assert(h2_bundle_segmented_manifest_parse((const uint8_t *)"format=1\n", 9u, &m) != H2_PAL_OK);
    assert(m.app.size == 0u);
    test_data_only_envelope(&mem);
    test_incompressible_members(&mem);
    return 0;
}
