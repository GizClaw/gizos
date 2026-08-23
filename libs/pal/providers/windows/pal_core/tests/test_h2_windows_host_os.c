#include "h2_windows_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct host_os_test_state {
    DWORD drives;
    UINT drive_types[26];
    DWORD attributes[26];
    h2_pal_result_t create_result;
    size_t mount_count;
    char sources[26][4];
    char targets[26][3];
} host_os_test_state_t;

static host_os_test_state_t test_state;

DWORD WINAPI h2_test_GetLogicalDrives(void) { return test_state.drives; }

UINT WINAPI h2_test_GetDriveTypeW(LPCWSTR root) {
    assert(root[0] >= L'A' && root[0] <= L'Z');
    assert(root[1] == L':' && root[2] == L'\\' && root[3] == L'\0');
    return test_state.drive_types[root[0] - L'A'];
}

DWORD WINAPI h2_test_GetFileAttributesW(LPCWSTR root) {
    assert(root[0] >= L'A' && root[0] <= L'Z');
    return test_state.attributes[root[0] - L'A'];
}

h2_pal_result_t h2_test_windows_platform_create(
    const h2_windows_platform_config_t *config,
    h2_windows_platform_t **out_platform) {
    assert(config != NULL);
    assert(out_platform != NULL);
    test_state.mount_count = config->fs_mount_count;
    for (size_t index = 0u; index < config->fs_mount_count; ++index) {
        assert(strlen(config->fs_sources[index]) == 3u);
        assert(strlen(config->fs_targets[index]) == 2u);
        memcpy(test_state.sources[index], config->fs_sources[index], 4u);
        memcpy(test_state.targets[index], config->fs_targets[index], 3u);
    }
    if (test_state.create_result == H2_PAL_OK) {
        *out_platform = (h2_windows_platform_t *)(uintptr_t)0x1234u;
    }
    return test_state.create_result;
}

static void reset_state(void) {
    memset(&test_state, 0, sizeof(test_state));
    for (size_t index = 0u; index < 26u; ++index) {
        test_state.drive_types[index] = DRIVE_NO_ROOT_DIR;
        test_state.attributes[index] = INVALID_FILE_ATTRIBUTES;
    }
}

static void test_logical_drive_api_errors(void) {
    assert(h2_windows_platform_create_with_logical_drives(NULL) ==
           H2_PAL_ERR_INVALID_ARG);

    reset_state();
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create_with_logical_drives(&platform) ==
           H2_PAL_ERR_NO_MEMORY);
}

static void test_fixed_removable_filtering_and_mount_mapping(void) {
    reset_state();
    test_state.drives = (1u << 0u) | (1u << 1u) | (1u << 2u) |
                        (1u << 3u) | (1u << 4u);
    test_state.drive_types[0] = DRIVE_FIXED;
    test_state.drive_types[1] = DRIVE_REMOVABLE;
    test_state.drive_types[2] = DRIVE_REMOTE;
    test_state.drive_types[3] = DRIVE_FIXED;
    test_state.drive_types[4] = DRIVE_FIXED;
    test_state.attributes[0] = FILE_ATTRIBUTE_DIRECTORY;
    test_state.attributes[1] = FILE_ATTRIBUTE_DIRECTORY;
    test_state.attributes[2] = FILE_ATTRIBUTE_DIRECTORY;
    test_state.attributes[3] =
        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    test_state.attributes[4] = INVALID_FILE_ATTRIBUTES;
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create_with_logical_drives(&platform) ==
           H2_PAL_OK);
    assert(platform != NULL);
    assert(test_state.mount_count == 2u);
    assert(strcmp(test_state.sources[0], "A:\\") == 0);
    assert(strcmp(test_state.targets[0], "/a") == 0);
    assert(strcmp(test_state.sources[1], "B:\\") == 0);
    assert(strcmp(test_state.targets[1], "/b") == 0);
}

static void test_remote_cd_reparse_and_inaccessible_drive_exclusion(void) {
    reset_state();
    test_state.drives = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 25u);
    test_state.drive_types[0] = DRIVE_REMOTE;
    test_state.drive_types[1] = DRIVE_FIXED;
    test_state.drive_types[2] = DRIVE_FIXED;
    test_state.drive_types[25] = DRIVE_CDROM;
    test_state.attributes[0] = FILE_ATTRIBUTE_DIRECTORY;
    test_state.attributes[1] =
        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    test_state.attributes[2] = INVALID_FILE_ATTRIBUTES;
    test_state.attributes[25] = FILE_ATTRIBUTE_DIRECTORY;
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create_with_logical_drives(&platform) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(platform == NULL);
    assert(test_state.mount_count == 0u);
}

static void test_platform_create_failure(void) {
    reset_state();
    test_state.drives = 1u;
    test_state.drive_types[0] = DRIVE_FIXED;
    test_state.attributes[0] = FILE_ATTRIBUTE_DIRECTORY;
    test_state.create_result = H2_PAL_ERR_NO_MEMORY;
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create_with_logical_drives(&platform) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(platform == NULL);
}

int main(void) {
    test_logical_drive_api_errors();
    test_fixed_removable_filtering_and_mount_mapping();
    test_remote_cd_reparse_and_inaccessible_drive_exclusion();
    test_platform_create_failure();
    return 0;
}
