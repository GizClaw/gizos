#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "h2_posix_pal_core.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct test_mount {
    const char *source;
    const char *target;
} test_mount_t;

static int test_create(const test_mount_t *mounts, size_t mount_count,
                       h2_posix_host_fs_t **out_fs) {
    if (mounts == NULL) {
        return h2_posix_host_fs_create(NULL, NULL, mount_count, out_fs);
    }
    const char **sources = calloc(mount_count, sizeof(*sources));
    const char **targets = calloc(mount_count, sizeof(*targets));
    assert(sources != NULL && targets != NULL);
    for (size_t index = 0u; index < mount_count; ++index) {
        sources[index] = mounts[index].source;
        targets[index] = mounts[index].target;
    }
    const int result = h2_posix_host_fs_create(sources, targets, mount_count,
                                               out_fs);
    free(targets);
    free(sources);
    return result;
}

static void make_directory(const char *path) {
    assert(mkdir(path, 0700) == 0);
}

static void join_path(char *out, size_t out_size, const char *left, const char *right) {
    int written = snprintf(out, out_size, "%s/%s", left, right);
    assert(written > 0 && (size_t)written < out_size);
}

int main(void) {
    h2_posix_host_fs_t *empty_fs = NULL;
    assert(test_create(NULL, 0u, &empty_fs) == H2_PAL_FS_OK);
    h2_pal_fs_stat_t stat_value;
    assert(h2_pal_fs_stat(h2_posix_host_fs_api(empty_fs), "/data", &stat_value) == H2_PAL_FS_ERR_NOT_FOUND);
    h2_posix_host_fs_destroy(empty_fs);
    assert(test_create(NULL, 1u, &empty_fs) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(empty_fs == NULL);

    char temp_template[] = "/tmp/h2-desktop-fs-XXXXXX";
    char *temp_root = mkdtemp(temp_template);
    assert(temp_root != NULL);
    char data_root[PATH_MAX];
    char dl_root[PATH_MAX];
    join_path(data_root, sizeof(data_root), temp_root, "data");
    join_path(dl_root, sizeof(dl_root), temp_root, "dl");
    make_directory(data_root);
    make_directory(dl_root);

    const test_mount_t single_mount = {.source = data_root, .target = "/data"};
    h2_posix_host_fs_t *fs = NULL;
    assert(test_create(&single_mount, 1u, &fs) == H2_PAL_FS_OK);
    h2_posix_host_fs_destroy(fs);

    assert(setenv("H2_DESKTOP_SOURCE_ROOT", "/build/worktree", 1) == 0);
    assert(setenv("H2_DESKTOP_RESOURCE_ROOT", temp_root, 1) == 0);
    const test_mount_t packaged_mount = {
        .source = "/build/worktree/data",
        .target = "/packaged",
    };
    assert(test_create(&packaged_mount, 1u, &fs) == H2_PAL_FS_OK);
    h2_posix_host_fs_destroy(fs);
    const test_mount_t prefix_escape_mount = {
        .source = "/build/worktree-other/data",
        .target = "/packaged",
    };
    assert(test_create(&prefix_escape_mount, 1u, &fs) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(unsetenv("H2_DESKTOP_SOURCE_ROOT") == 0);
    assert(unsetenv("H2_DESKTOP_RESOURCE_ROOT") == 0);

    const test_mount_t mounts[] = {
        {.source = data_root, .target = "/data"},
        {.source = dl_root, .target = "/dl"},
    };
    fs = NULL;
    assert(test_create(mounts, 2u, &fs) == H2_PAL_FS_OK);
    const h2_pal_fs_api_t *api = h2_posix_host_fs_api(fs);
    assert(api != NULL);

    assert(h2_pal_fs_mkdir(api, "/data/nested") == H2_PAL_FS_OK);
    assert(h2_pal_fs_mkdir(api, "/data/nested") == H2_PAL_FS_OK);
    h2_pal_fs_file_t *file = NULL;
    assert(h2_pal_fs_open(api, "/data/nested/value.txt", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_FS_OK);
    const char payload[] = "desktop filesystem";
    size_t written = 0u;
    assert(h2_pal_fs_write(api, file, payload, sizeof(payload), &written) == H2_PAL_FS_OK);
    assert(written == sizeof(payload));
    assert(h2_pal_fs_sync(api, file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_close(api, file) == H2_PAL_FS_OK);

    assert(h2_pal_fs_stat(api, "/data/nested/value.txt", &stat_value) == H2_PAL_FS_OK);
    assert(!stat_value.is_dir && stat_value.size == sizeof(payload));
    assert(h2_pal_fs_open(api, "/data/nested/value.txt", H2_PAL_FS_OPEN_READ, &file) == H2_PAL_FS_OK);
    char read_buffer[sizeof(payload)] = {0};
    size_t read_len = 0u;
    assert(h2_pal_fs_read(api, file, read_buffer, sizeof(read_buffer), &read_len) == H2_PAL_FS_OK);
    assert(read_len == sizeof(read_buffer) && memcmp(read_buffer, payload, sizeof(payload)) == 0);
    memset(read_buffer, 0, sizeof(read_buffer));
    assert(h2_pal_fs_seek(api, file, 0u) == H2_PAL_FS_OK);
    assert(h2_pal_fs_read(api, file, read_buffer, sizeof(read_buffer), &read_len) == H2_PAL_FS_OK);
    assert(read_len == sizeof(read_buffer) && memcmp(read_buffer, payload, sizeof(payload)) == 0);
    assert(h2_pal_fs_seek(api, file, UINT64_MAX) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(h2_pal_fs_close(api, file) == H2_PAL_FS_OK);

    assert(h2_pal_fs_open(api, "/data/nested/value.txt", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_FS_OK);
    const char shorter_payload[] = "short";
    assert(h2_pal_fs_write(api, file, shorter_payload, sizeof(shorter_payload), &written) == H2_PAL_FS_OK);
    assert(written == sizeof(shorter_payload));
    assert(h2_pal_fs_close(api, file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_stat(api, "/data/nested/value.txt", &stat_value) == H2_PAL_FS_OK);
    assert(stat_value.size == sizeof(shorter_payload));

    assert(h2_pal_fs_rename(api, "/data/nested/value.txt", "/data/nested/renamed.txt") == H2_PAL_FS_OK);
    assert(h2_pal_fs_rename(api, "/data/nested/renamed.txt", "/dl/renamed.txt") == H2_PAL_FS_ERR_UNSUPPORTED);
    assert(h2_pal_fs_stat(api, "/data/nested/renamed.txt", &stat_value) == H2_PAL_FS_OK);

    char old_cwd[PATH_MAX];
    assert(getcwd(old_cwd, sizeof(old_cwd)) != NULL);
    assert(chdir("/") == 0);
    assert(h2_pal_fs_stat(api, "/data/nested/renamed.txt", &stat_value) == H2_PAL_FS_OK);
    assert(chdir(old_cwd) == 0);

    assert(h2_pal_fs_open(api, "/data/../dl/file", H2_PAL_FS_OPEN_READ, &file) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(h2_pal_fs_stat(api, "/data/", &stat_value) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(h2_pal_fs_stat(api, "/unknown/file", &stat_value) == H2_PAL_FS_ERR_NOT_FOUND);
    assert(h2_pal_fs_stat(api, "/database/file", &stat_value) == H2_PAL_FS_ERR_NOT_FOUND);
    char symlink_path[PATH_MAX];
    join_path(symlink_path, sizeof(symlink_path), data_root, "escape");
    assert(symlink(dl_root, symlink_path) == 0);
    assert(h2_pal_fs_stat(api, "/data/escape", &stat_value) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(h2_pal_fs_stat(api, "/data/escape/file", &stat_value) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(unlink(symlink_path) == 0);

    assert(h2_pal_fs_open(api, "/data/plain-file", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_close(api, file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_stat(api, "/data/plain-file/child", &stat_value) == H2_PAL_FS_ERR_IO);
    assert(h2_pal_fs_remove(api, "/data/plain-file") == H2_PAL_FS_OK);

    assert(h2_pal_fs_remove(api, "/data/nested/renamed.txt") == H2_PAL_FS_OK);
    assert(h2_pal_fs_remove(api, "/data/nested") == H2_PAL_FS_OK);
    assert(h2_pal_fs_mkdir(api, "/data/clear-me") == H2_PAL_FS_OK);
    assert(h2_pal_fs_mkdir(api, "/data/clear-me/child") == H2_PAL_FS_OK);
    assert(h2_pal_fs_open(api, "/data/clear-me/child/value", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_close(api, file) == H2_PAL_FS_OK);
    assert(h2_pal_fs_clear(api, "/data/clear-me") == H2_PAL_FS_OK);
    assert(h2_pal_fs_stat(api, "/data/clear-me", &stat_value) == H2_PAL_FS_OK && stat_value.is_dir);
    assert(h2_pal_fs_stat(api, "/data/clear-me/child", &stat_value) == H2_PAL_FS_ERR_NOT_FOUND);
    assert(h2_pal_fs_remove(api, "/data/clear-me") == H2_PAL_FS_OK);

    h2_posix_host_fs_destroy(fs);

    const test_mount_t overlapping_targets[] = {
        {.source = data_root, .target = "/data"},
        {.source = data_root, .target = "/data/cache"},
    };
    assert(test_create(overlapping_targets, 2u, &fs) == H2_PAL_FS_ERR_INVALID_ARG);
    const test_mount_t duplicate_targets[] = {
        {.source = data_root, .target = "/data"},
        {.source = dl_root, .target = "/data"},
    };
    assert(test_create(duplicate_targets, 2u, &fs) == H2_PAL_FS_ERR_INVALID_ARG);

    char source_symlink[PATH_MAX];
    join_path(source_symlink, sizeof(source_symlink), temp_root, "source-link");
    assert(symlink(data_root, source_symlink) == 0);
    const test_mount_t symlink_source = {.source = source_symlink, .target = "/data"};
    assert(test_create(&symlink_source, 1u, &fs) == H2_PAL_FS_ERR_INVALID_ARG);
    assert(unlink(source_symlink) == 0);

    char canonical_root[PATH_MAX];
    join_path(canonical_root, sizeof(canonical_root), data_root, "canonical-root");
    make_directory(canonical_root);
    char ancestor_symlink[PATH_MAX];
    join_path(ancestor_symlink, sizeof(ancestor_symlink), temp_root, "source-parent-link");
    assert(symlink(data_root, ancestor_symlink) == 0);
    char source_with_symlink_ancestor[PATH_MAX];
    join_path(source_with_symlink_ancestor, sizeof(source_with_symlink_ancestor), ancestor_symlink, "canonical-root");
    const test_mount_t canonical_mount = {
        .source = source_with_symlink_ancestor,
        .target = "/canonical",
    };
    assert(test_create(&canonical_mount, 1u, &fs) == H2_PAL_FS_OK);
    api = h2_posix_host_fs_api(fs);
    assert(unlink(ancestor_symlink) == 0);
    assert(symlink(dl_root, ancestor_symlink) == 0);
    assert(h2_pal_fs_mkdir(api, "/canonical/kept") == H2_PAL_FS_OK);
    char canonical_child[PATH_MAX];
    join_path(canonical_child, sizeof(canonical_child), canonical_root, "kept");
    assert(access(canonical_child, F_OK) == 0);
    assert(h2_pal_fs_remove(api, "/canonical/kept") == H2_PAL_FS_OK);
    h2_posix_host_fs_destroy(fs);
    assert(unlink(ancestor_symlink) == 0);
    assert(rmdir(canonical_root) == 0);

    if (access("/dev/full", F_OK) == 0) {
        const test_mount_t full_mount = {.source = "/dev", .target = "/host-device"};
        assert(test_create(&full_mount, 1u, &fs) == H2_PAL_FS_OK);
        api = h2_posix_host_fs_api(fs);
        assert(h2_pal_fs_open(api, "/host-device/full", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_FS_OK);
        char full_payload[65536] = {0};
        int write_rc = h2_pal_fs_write(api, file, full_payload, sizeof(full_payload), &written);
        int sync_rc = write_rc == H2_PAL_FS_OK ? h2_pal_fs_sync(api, file) : write_rc;
        assert(write_rc == H2_PAL_FS_ERR_NO_SPACE || sync_rc == H2_PAL_FS_ERR_NO_SPACE);
        int close_rc = h2_pal_fs_close(api, file);
        assert(close_rc == H2_PAL_FS_OK || close_rc == H2_PAL_FS_ERR_NO_SPACE);
        h2_posix_host_fs_destroy(fs);
    }
    assert(rmdir(data_root) == 0);
    assert(rmdir(dl_root) == 0);
    assert(rmdir(temp_root) == 0);

    return 0;
}
