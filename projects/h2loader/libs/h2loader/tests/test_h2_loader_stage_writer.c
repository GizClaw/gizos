#include "h2_loader_stage.h"
#include <assert.h>
#include <string.h>

static const char checksum[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
struct h2_pal_fs_file { int unused; };
static struct h2_pal_fs_file file;
static int short_write, verify_error, commit_error, stage_valid, aborts, closes;
static h2_loader_package_inspection_t inspection;
static int open_file(void *user, const char *path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t **out) {
    (void)user; (void)path;
    assert(!stage_valid && mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE);
    *out = &file;
    return H2_PAL_OK;
}
static int write_file(void *user, h2_pal_fs_file_t *handle, const void *data,
                      size_t size, size_t *out) {
    (void)user; (void)handle; (void)data;
    *out = short_write && size ? size - 1u : size;
    return H2_PAL_OK;
}
static int sync_file(void *user, h2_pal_fs_file_t *handle) {
    (void)user; (void)handle;
    return H2_PAL_OK;
}
static int close_file(void *user, h2_pal_fs_file_t *handle) {
    (void)user; (void)handle;
    closes++;
    return H2_PAL_OK;
}
int h2_loader_stage_begin(const h2_pal_pref_api_t *pref) {
    (void)pref;
    stage_valid = 0;
    return H2_PAL_OK;
}
int h2_loader_stage_abort(const h2_pal_fs_api_t *fs,
                          const h2_pal_pref_api_t *pref, const char *path) {
    (void)fs; (void)pref; (void)path;
    stage_valid = 0;
    aborts++;
    return H2_PAL_OK;
}
int h2_loader_package_verify_path(h2_loader_package_t *package, const char *path,
                                  uint64_t size, const char *sha) {
    (void)package; (void)path; (void)size;
    assert(!strcmp(sha, checksum));
    return verify_error;
}
int h2_loader_package_inspect_path(h2_loader_package_t *package, const char *path,
                                   h2_loader_package_inspection_t *out) {
    (void)package; (void)path;
    *out = inspection;
    return H2_PAL_OK;
}
int h2_loader_stage_commit_inspection(const h2_pal_pref_api_t *pref,
    uint64_t size, const char *sha, const h2_loader_package_inspection_t *info,
    h2_loader_metadata_t *out) {
    (void)pref; (void)size; (void)sha; (void)info; (void)out;
    if (!commit_error) stage_valid = 1;
    return commit_error;
}
int main(void) {
    const h2_pal_fs_vtable_t vtable = {
        .open = open_file, .write = write_file, .sync = sync_file, .close = close_file,
    };
    const h2_pal_fs_api_t fs = {.vtable = &vtable};
    const h2_pal_pref_api_t pref = {0};
    h2_loader_package_t package = {.config = {.fs = &fs, .package_path = "/dl/pkg"}};
    h2_loader_stage_writer_t writer = {0};
    inspection.manifest.role = H2_LOADER_IMAGE_ROLE_APP;
    strcpy(inspection.manifest.board, "test");
    strcpy(inspection.manifest.target, "host");
    inspection.data_checksum_len = 64;
    memcpy(inspection.data_checksum, checksum, 64);
    const h2_loader_package_inspection_t *result = NULL;
    unsigned percent = 0;
    const uint8_t bytes[4] = {0};
    assert(h2_loader_stage_writer_commit(&writer) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_loader_stage_writer_begin(&writer, &package, &pref, 4, checksum, "test", "host") == 0);
    assert(h2_loader_stage_writer_begin(&writer, &package, &pref, 4, checksum, "test", "host") == H2_PAL_ERR_BUSY);
    for (unsigned i = 1; i <= 4; i++) {
        assert(h2_loader_stage_writer_write(&writer, bytes, 1, &percent) == 0);
        assert(percent == i * 25u && !stage_valid);
    }
    assert(h2_loader_stage_writer_inspect(&writer, &result) == 0 && result && !stage_valid);
    commit_error = H2_PAL_ERR_IO;
    assert(h2_loader_stage_writer_commit(&writer) == commit_error && !stage_valid);
    commit_error = 0;
    assert(h2_loader_stage_writer_commit(&writer) == 0 && stage_valid);
    assert(h2_loader_stage_writer_abort(&writer) == 0 && !stage_valid && closes == 1);
    for (int failure = 0; failure < 6; failure++) {
        assert(h2_loader_stage_writer_begin(&writer, &package, &pref, 4, checksum, "test", "host") == 0);
        short_write = failure == 0;
        verify_error = failure == 1 ? H2_PAL_ERR_FORMAT : 0;
        inspection.manifest.role = failure == 2 ? H2_LOADER_IMAGE_ROLE_H2LOADER : H2_LOADER_IMAGE_ROLE_APP;
        strcpy(inspection.manifest.board, failure == 4 ? "wrong-board" : "test");
        strcpy(inspection.manifest.target, failure == 5 ? "wrong-target" : "host");
        int written = h2_loader_stage_writer_write(&writer, bytes, failure == 3 ? 3 : 4, &percent);
        if (short_write) assert(written == H2_PAL_ERR_IO);
        assert(h2_loader_stage_writer_inspect(&writer, &result) != 0);
        assert(h2_loader_stage_writer_commit(&writer) == H2_PAL_ERR_INVALID_STATE && !stage_valid);
        assert(h2_loader_stage_writer_abort(&writer) == 0);
    }
    short_write = 0;
    assert(h2_loader_stage_writer_begin(&writer, &package, &pref, UINT64_MAX, checksum, "test", "host") == 0);
    assert(h2_loader_stage_writer_write(&writer, bytes, 4, &percent) == 0 && percent == 0);
    assert(h2_loader_stage_writer_abort(&writer) == 0 && aborts == 8);
    assert(h2_loader_stage_writer_abort(&writer) == 0 && aborts == 8);
    return 0;
}
