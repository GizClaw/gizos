#include "h2_windows_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>

#ifdef H2_WINDOWS_HOST_OS_TESTING
DWORD WINAPI h2_test_GetLogicalDrives(void);
UINT WINAPI h2_test_GetDriveTypeW(LPCWSTR);
DWORD WINAPI h2_test_GetFileAttributesW(LPCWSTR);
h2_pal_result_t h2_test_windows_platform_create(
    const h2_windows_platform_config_t *, h2_windows_platform_t **);
#endif

#ifndef H2_WINDOWS_GET_LOGICAL_DRIVES
#define H2_WINDOWS_GET_LOGICAL_DRIVES GetLogicalDrives
#endif
#ifndef H2_WINDOWS_GET_DRIVE_TYPE
#define H2_WINDOWS_GET_DRIVE_TYPE GetDriveTypeW
#endif
#ifndef H2_WINDOWS_GET_FILE_ATTRIBUTES
#define H2_WINDOWS_GET_FILE_ATTRIBUTES GetFileAttributesW
#endif
#ifndef H2_WINDOWS_PLATFORM_CREATE
#define H2_WINDOWS_PLATFORM_CREATE h2_windows_platform_create
#endif

h2_pal_result_t h2_windows_platform_create_with_logical_drives(
    h2_windows_platform_t **out_platform) {
    if (out_platform == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_platform = NULL;
    DWORD drives = H2_WINDOWS_GET_LOGICAL_DRIVES();
    if (drives == 0u) {
        return GetLastError() == ERROR_NOT_ENOUGH_MEMORY
                   ? H2_PAL_ERR_NO_MEMORY
                   : H2_PAL_ERR_IO;
    }

    char source_storage[26][4];
    char target_storage[26][3];
    const char *sources[26];
    const char *targets[26];
    size_t count = 0u;
    for (size_t index = 0u; index < 26u; ++index) {
        if ((drives & (1u << index)) == 0u) {
            continue;
        }
        wchar_t root[] = {(wchar_t)(L'A' + index), L':', L'\\', L'\0'};
        UINT drive_type = H2_WINDOWS_GET_DRIVE_TYPE(root);
        if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE) {
            continue;
        }
        DWORD attributes = H2_WINDOWS_GET_FILE_ATTRIBUTES(root);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            continue;
        }
        source_storage[count][0] = (char)('A' + (int)index);
        source_storage[count][1] = ':';
        source_storage[count][2] = '\\';
        source_storage[count][3] = '\0';
        target_storage[count][0] = '/';
        target_storage[count][1] = (char)('a' + (int)index);
        target_storage[count][2] = '\0';
        sources[count] = source_storage[count];
        targets[count] = target_storage[count];
        ++count;
    }
    if (count == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    const h2_windows_platform_config_t config = {
        .fs_sources = sources,
        .fs_targets = targets,
        .fs_mount_count = count,
    };
    return H2_WINDOWS_PLATFORM_CREATE(&config, out_platform);
}
