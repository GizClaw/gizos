#include "h2_windows_platform.h"

#include <assert.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

typedef struct test_mount_point_buffer {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t paths[MAX_PATH * 2u + 8u];
} test_mount_point_buffer_t;

static int create_directory_junction(const wchar_t *link_path,
                                     const wchar_t *target_path) {
    if (!CreateDirectoryW(link_path, NULL)) {
        return 0;
    }
    HANDLE link = CreateFileW(
        link_path, GENERIC_WRITE, 0u, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (link == INVALID_HANDLE_VALUE) {
        (void)RemoveDirectoryW(link_path);
        return 0;
    }
    static const wchar_t prefix[] = L"\\??\\";
    size_t prefix_len = (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    size_t target_len = wcslen(target_path);
    size_t substitute_len = prefix_len + target_len;
    test_mount_point_buffer_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    assert(substitute_len + 1u + target_len + 1u <=
           sizeof(buffer.paths) / sizeof(buffer.paths[0]));
    memcpy(buffer.paths, prefix, prefix_len * sizeof(wchar_t));
    memcpy(buffer.paths + prefix_len, target_path,
           target_len * sizeof(wchar_t));
    memcpy(buffer.paths + substitute_len + 1u, target_path,
           target_len * sizeof(wchar_t));
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_length = (WORD)(substitute_len * sizeof(wchar_t));
    buffer.print_offset =
        (WORD)((substitute_len + 1u) * sizeof(wchar_t));
    buffer.print_length = (WORD)(target_len * sizeof(wchar_t));
    buffer.data_length =
        (WORD)(8u + buffer.print_offset + buffer.print_length +
               sizeof(wchar_t));
    DWORD returned = 0u;
    BOOL configured = DeviceIoControl(
        link, FSCTL_SET_REPARSE_POINT, &buffer,
        8u + buffer.data_length, NULL, 0u, &returned, NULL);
    (void)CloseHandle(link);
    if (!configured) {
        (void)RemoveDirectoryW(link_path);
    }
    return configured != FALSE;
}

int main(void) {
    wchar_t root[MAX_PATH];
    wchar_t path[MAX_PATH];
    wchar_t outside[MAX_PATH];
    wchar_t outside_marker[MAX_PATH];
    wchar_t junction[MAX_PATH];
    char utf8[MAX_PATH * 4u];
    assert(GetTempPathW(MAX_PATH, root) != 0u);
    assert(GetTempFileNameW(root, L"h2f", 0u, path) != 0u);
    assert(DeleteFileW(path));
    assert(CreateDirectoryW(path, NULL));
    assert(GetTempFileNameW(root, L"h2o", 0u, outside) != 0u);
    assert(DeleteFileW(outside));
    assert(CreateDirectoryW(outside, NULL));
    assert(wcscpy_s(outside_marker, MAX_PATH, outside) == 0);
    assert(wcscat_s(outside_marker, MAX_PATH, L"\\marker.txt") == 0);
    HANDLE marker = CreateFileW(outside_marker, GENERIC_WRITE, 0u, NULL,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(marker != INVALID_HANDLE_VALUE);
    assert(CloseHandle(marker));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                               utf8, sizeof(utf8), NULL, NULL) > 0);
    const char *sources[] = {utf8};
    const char *targets[] = {"/data"};
    h2_windows_platform_config_t config = {
        .fs_sources = sources,
        .fs_targets = targets,
        .fs_mount_count = 1u,
    };
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&config, &platform) == H2_PAL_OK);
    const h2_pal_fs_api_t *fs = h2_windows_fs_api(platform);
    h2_pal_fs_file_t *file = NULL;
    assert(h2_pal_fs_open(fs, "/data/中文.txt", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &file) == H2_PAL_OK);
    const char payload[] = "unicode";
    size_t written = 0u;
    assert(h2_pal_fs_write(fs, file, payload, sizeof(payload), &written) ==
           H2_PAL_OK);
    assert(written == sizeof(payload));
    assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_rename(fs, "/data/中文.txt", "/data/renamed.txt") ==
           H2_PAL_OK);
    h2_pal_fs_stat_t stat_value;
    assert(h2_pal_fs_stat(fs, "/data/renamed.txt", &stat_value) == H2_PAL_OK);
    assert(h2_pal_fs_stat(fs, "/data/../escape", &stat_value) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_fs_open(fs, "/data/NUL.txt", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &file) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_fs_open(fs, "/data/trailing.",
                          H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &file) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_fs_remove(fs, "/data/renamed.txt") == H2_PAL_OK);
    assert(h2_pal_fs_mkdir(fs, "/data/clear-me") == H2_PAL_OK);
    assert(h2_pal_fs_open(fs, "/data/clear-me/value.txt",
                          H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &file) == H2_PAL_OK);
    assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_clear(fs, "/data/clear-me") == H2_PAL_OK);
    assert(h2_pal_fs_stat(fs, "/data/clear-me/value.txt", &stat_value) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(h2_pal_fs_remove(fs, "/data/clear-me") == H2_PAL_OK);
    assert(h2_pal_fs_remove(fs, "/data") == H2_PAL_ERR_INVALID_ARG);
    assert(wcscpy_s(junction, MAX_PATH, path) == 0);
    assert(wcscat_s(junction, MAX_PATH, L"\\junction") == 0);
    assert(create_directory_junction(junction, outside));
    assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_INVALID_ARG);
    assert(GetFileAttributesW(outside_marker) != INVALID_FILE_ATTRIBUTES);
    assert(RemoveDirectoryW(junction));
    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    assert(RemoveDirectoryW(path));
    assert(DeleteFileW(outside_marker));
    assert(RemoveDirectoryW(outside));
    return 0;
}
