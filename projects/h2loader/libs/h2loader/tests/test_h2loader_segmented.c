#include "h2_loader_package.h"
#include "h2_loader_sha256.h"
#include "h2_h2loader_host_package.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory_handle { int kind; size_t offset; } memory_handle_t;
#define DATA_SIZE 5000u
typedef struct fixture {
    uint8_t package[32768];
    size_t package_len;
    uint8_t app[64], data[DATA_SIZE], checksum[65];
    size_t data_len, checksum_len, app_offset;
    unsigned app_begins, app_finishes, app_aborts, data_writes, data_clears;
    unsigned data_partial_progress;
    int deny_alloc;
    memory_handle_t handles[3];
    h2_loader_sha256_t sha;
} fixture_t;

static const uint8_t new_app[] = "new app";
static uint8_t new_data[DATA_SIZE + 1u];

static void progress(void *user, h2_loader_install_phase_t phase,
    uint64_t completed, uint64_t total, const char *detail) {
    fixture_t *f = user;
    if (phase == H2_LOADER_INSTALL_PHASE_DATA && strcmp(detail, "test.txt") == 0) {
        assert(total == DATA_SIZE && completed <= total);
        if (completed != 0u && completed < total) ++f->data_partial_progress;
    }
}

static void *allocate(void *user, size_t len) {
    return ((fixture_t *)user)->deny_alloc ? NULL : malloc(len);
}
static void release(void *user, void *ptr) { (void)user; free(ptr); }

static int source_read(void *user, uint64_t offset, uint8_t *out, size_t cap, size_t *got) {
    const uint8_t *source = user;
    size_t len = strlen((const char *)source);
    if (offset > len) return H2_PAL_ERR_INVALID_ARG;
    *got = len - (size_t)offset < cap ? len - (size_t)offset : cap;
    memcpy(out, source + offset, *got);
    return H2_PAL_OK;
}
static int package_write(void *user, const uint8_t *data, size_t len) {
    fixture_t *f = user;
    if (len > sizeof(f->package) - f->package_len) return H2_PAL_ERR_NO_SPACE;
    memcpy(f->package + f->package_len, data, len);
    f->package_len += len;
    return H2_PAL_OK;
}

static int fs_open(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out) {
    fixture_t *f = user;
    int kind;
    if (strcmp(path, "/dl/update.tar.zlib") == 0) kind = 1;
    else if (strcmp(path, "/data/.checksum") == 0) kind = 2;
    else if (strcmp(path, "/data/test.txt") == 0) kind = 3;
    else return H2_PAL_FS_ERR_NOT_FOUND;
    if (kind == 2 && mode == H2_PAL_FS_OPEN_READ && f->checksum_len == 0u)
        return H2_PAL_FS_ERR_NOT_FOUND;
    if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
        if (kind == 2) f->checksum_len = 0u;
        if (kind == 3) f->data_len = 0u;
    }
    f->handles[kind - 1] = (memory_handle_t){.kind = kind};
    *out = (h2_pal_fs_file_t *)&f->handles[kind - 1];
    return H2_PAL_OK;
}
static int fs_read(void *user, h2_pal_fs_file_t *file, void *out, size_t cap, size_t *got) {
    fixture_t *f = user;
    memory_handle_t *h = (memory_handle_t *)file;
    const uint8_t *data = h->kind == 1 ? f->package : f->checksum;
    size_t len = h->kind == 1 ? f->package_len : f->checksum_len;
    assert(h->offset <= len);
    *got = len - h->offset < cap ? len - h->offset : cap;
    /* Deliberately fragment filesystem reads, including format detection. */
    if (*got > 137u) *got = 137u;
    memcpy(out, data + h->offset, *got);
    h->offset += *got;
    return H2_PAL_OK;
}
static int fs_seek(void *user, h2_pal_fs_file_t *file, uint64_t offset) {
    fixture_t *f = user;
    memory_handle_t *h = (memory_handle_t *)file;
    size_t len = h->kind == 1 ? f->package_len : f->checksum_len;
    if (offset > len) return H2_PAL_ERR_INVALID_ARG;
    h->offset = (size_t)offset;
    return H2_PAL_OK;
}
static int fs_write(void *user, h2_pal_fs_file_t *file, const void *data, size_t len, size_t *written) {
    fixture_t *f = user;
    memory_handle_t *h = (memory_handle_t *)file;
    uint8_t *out = h->kind == 2 ? f->checksum : f->data;
    size_t cap = h->kind == 2 ? sizeof(f->checksum) : sizeof(f->data);
    size_t *size = h->kind == 2 ? &f->checksum_len : &f->data_len;
    if (len > cap - *size) return H2_PAL_ERR_NO_SPACE;
    memcpy(out + *size, data, len);
    *size += len;
    *written = len;
    if (h->kind == 3) ++f->data_writes;
    return H2_PAL_OK;
}
static int fs_stat(void *user, const char *path, h2_pal_fs_stat_t *out) {
    assert(strcmp(path, "/dl/update.tar.zlib") == 0);
    *out = (h2_pal_fs_stat_t){.size = ((fixture_t *)user)->package_len};
    return H2_PAL_OK;
}
static int fs_remove(void *user, const char *path) {
    assert(strcmp(path, "/data/.checksum") == 0);
    ((fixture_t *)user)->checksum_len = 0u;
    return H2_PAL_OK;
}
static int fs_close(void *user, h2_pal_fs_file_t *file) { (void)user; (void)file; return H2_PAL_OK; }
static int fs_mkdir(void *user, const char *path) { (void)user; (void)path; return H2_PAL_OK; }
static int clear_data(void *user, const char *path) {
    fixture_t *f = user;
    assert(strcmp(path, "/data") == 0);
    ++f->data_clears;
    f->data_len = f->checksum_len = 0u;
    return H2_PAL_OK;
}
static int digest_start(void *user) { h2_loader_sha256_init(&((fixture_t *)user)->sha); return H2_PAL_OK; }
static int digest_update(void *user, const uint8_t *data, size_t len) {
    h2_loader_sha256_update(&((fixture_t *)user)->sha, data, len); return H2_PAL_OK;
}
static int digest_finish(void *user, uint8_t out[32]) {
    h2_loader_sha256_finish(&((fixture_t *)user)->sha, out); return H2_PAL_OK;
}
static void digest_abort(void *user) { memset(&((fixture_t *)user)->sha, 0, sizeof(h2_loader_sha256_t)); }
static int capacity(void *user, uint32_t id, uint64_t *out) {
    (void)user; assert(id == 2u); *out = 64u; return H2_PAL_OK;
}
static int image_read(void *user, uint32_t id, uint64_t offset, void *out, size_t len) {
    fixture_t *f = user; assert(id == 2u);
    if (offset > sizeof(f->app) || len > sizeof(f->app) - offset) return H2_PAL_ERR_FORMAT;
    memcpy(out, f->app + offset, len); return H2_PAL_OK;
}
static int image_begin(void *user, uint32_t id, const h2_loader_image_identity_t *image) {
    fixture_t *f = user; assert(id == 2u && image->image_size == sizeof(new_app) - 1u);
    ++f->app_begins; f->app_offset = 0u; return H2_PAL_OK;
}
static int image_write(void *user, const void *data, size_t len) {
    fixture_t *f = user;
    assert(len <= sizeof(f->app) - f->app_offset);
    memcpy(f->app + f->app_offset, data, len); f->app_offset += len; return H2_PAL_OK;
}
static int image_finish(void *user, const h2_loader_image_identity_t *image) {
    fixture_t *f = user; assert(f->app_offset == image->image_size);
    ++f->app_finishes; return H2_PAL_OK;
}
static void image_abort(void *user) { ++((fixture_t *)user)->app_aborts; }

static void checksum(const uint8_t *data, size_t len, char out[65]) {
    h2_loader_sha256_t sha; uint8_t hash[32];
    h2_loader_sha256_init(&sha); h2_loader_sha256_update(&sha, data, len);
    h2_loader_sha256_finish(&sha, hash); h2_loader_sha256_hex(hash, out);
}

static void run_case(int same_app, int same_data, int broken_member) {
    fixture_t f = {0};
    const h2_pal_mem_vtable_t mv = {.alloc = allocate, .free = release};
    const h2_pal_mem_api_t mem = {.user = &f, .vtable = &mv};
    const h2_pal_fs_vtable_t fv = {.open = fs_open, .read = fs_read, .seek = fs_seek,
        .write = fs_write, .close = fs_close, .stat = fs_stat, .remove = fs_remove, .mkdir = fs_mkdir};
    const h2_pal_fs_api_t fs = {.user = &f, .vtable = &fv};
    const h2_loader_image_reader_vtable_t rv = {.get_capacity = capacity, .read = image_read};
    const h2_loader_image_reader_api_t reader = {.user = &f, .vtable = &rv};
    const h2_loader_image_writer_vtable_t wv = {.get_capacity = capacity, .begin = image_begin,
        .write = image_write, .finish = image_finish, .abort = image_abort};
    const h2_loader_image_writer_api_t writer = {.user = &f, .vtable = &wv};
    const h2_h2loader_host_package_source_t data = {.name = "data/test.txt",
        .size = sizeof(new_data) - 1u, .read = source_read, .user = (void *)new_data};
    const h2_h2loader_host_package_writer_config_t output = {.allocator = &mem,
        .role = "app", .board = "fixture", .target = "host", .version = "1",
        .app = {.name = "app/esp/app.bin", .size = sizeof(new_app) - 1u,
            .read = source_read, .user = (void *)new_app}, .data_entries = &data,
        .data_entry_count = 1u, .write = package_write, .write_user = &f, .package_format = 2u};
    h2_h2loader_host_package_writer_result_t produced;
    assert(h2_h2loader_host_package_write(&output, &produced) == H2_PAL_OK);
    memcpy(f.app, new_app, sizeof(new_app) - 1u);
    if (!same_app) f.app[0] ^= 1u;
    memcpy(f.checksum, produced.data_sha256, 64u); f.checksum[64] = '\n'; f.checksum_len = 65u;
    if (!same_data) f.checksum[0] = f.checksum[0] == 'a' ? 'b' : 'a';
    h2_loader_package_config_t config = {.fs = &fs, .allocator = &mem,
        .digest = {.user = &f, .start = digest_start, .update = digest_update,
            .finish = digest_finish, .abort = digest_abort},
        .image_reader = &reader, .image_writer = &writer,
        .progress = progress, .progress_user = &f,
        .clear_data = clear_data, .clear_data_user = &f};
    h2_loader_package_t package;
    h2_loader_package_inspection_t inspection;
    assert(h2_loader_package_init(&package, &config) == H2_PAL_OK);
    assert(h2_loader_package_inspect_path(&package, package.config.package_path, &inspection) == H2_PAL_OK);
    assert(inspection.manifest.format == 2u && inspection.data_checksum_len == 65u);
    assert(inspection.data_bytes == DATA_SIZE && inspection.pixa_bytes == 0u);
    if (broken_member != 0) {
        if (broken_member == 3) {
            char *value = strstr((char *)f.package + 512u, "data_sha256=");
            assert(value != NULL);
            value += strlen("data_sha256=");
            value[0] = value[0] == 'a' ? 'b' : 'a';
            f.checksum[0] = value[0] == 'c' ? 'd' : 'c';
        } else {
        const h2_bundle_segment_t *segment = broken_member == 1
            ? &inspection.segments.app : &inspection.segments.data;
        f.package[segment->offset] = 0u;
        char hex[65];
        checksum(f.package + segment->offset, (size_t)segment->compressed_size, hex);
        const char *key = broken_member == 1 ? "app_zlib_sha256=" : "data_zlib_sha256=";
        char *value = strstr((char *)f.package + 512u, key);
        assert(value != NULL); memcpy(value + strlen(key), hex, 64u);
        }
        assert(h2_loader_package_inspect_path(&package, package.config.package_path, &inspection) == H2_PAL_OK);
    }
    inspection.staged.valid = 1; inspection.staged.size = f.package_len;
    checksum(f.package, f.package_len, inspection.staged.checksum);
    h2_loader_package_install_plan_t plan;
    assert(h2_loader_package_plan_install(&package, &inspection, 2u, &plan) == H2_PAL_OK);
    assert(plan.update_app == !same_app && plan.update_data == !same_data);
    if (same_app && same_data) f.deny_alloc = 1; /* No inflater may be initialized. */
    h2_loader_package_install_result_t installed;
    int rc = h2_loader_package_install_to(&package, &inspection, 2u, &plan, &installed);
    int fails = (broken_member == 1 && !same_app) ||
        ((broken_member == 2 || broken_member == 3) && !same_data);
    if (fails) {
        assert(rc != H2_PAL_OK);
        if (broken_member == 1) assert(f.app_aborts == 1u && f.app_finishes == 0u);
        else assert(f.checksum_len == 0u);
        assert(installed.app_written == 0 && installed.data_written == 0);
    } else {
        assert(rc == H2_PAL_OK);
        assert(installed.app_written == !same_app && installed.data_written == !same_data);
        assert(f.app_begins == (unsigned)!same_app && f.app_finishes == (unsigned)!same_app);
        assert(f.data_clears == (unsigned)!same_data && (f.data_writes != 0u) == !same_data);
        if (!same_data) assert(f.data_partial_progress > 0u);
        assert(f.checksum_len == 65u && memcmp(f.checksum, produced.data_sha256, 64u) == 0);
        assert(memcmp(f.app, new_app, sizeof(new_app) - 1u) == 0);
        assert(f.app_aborts == 0u);
    }
}

int main(void) {
    uint32_t state = 0x12345678u;
    for (size_t i = 0u; i < DATA_SIZE; ++i) {
        state ^= state << 13u; state ^= state >> 17u; state ^= state << 5u;
        new_data[i] = (uint8_t)(1u + state % 255u);
    }
    for (int app = 0; app <= 1; ++app)
        for (int data = 0; data <= 1; ++data) run_case(app, data, 0);
    /* A deliberately invalid skipped stream proves that it was not inflated. */
    run_case(1, 0, 1);
    run_case(0, 1, 2);
    run_case(1, 1, 1);
    run_case(0, 0, 1);
    run_case(0, 0, 2);
    run_case(0, 0, 3);
    return 0;
}
