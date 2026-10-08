#include "h2_h2loader_cli_target.h"
#include "h2_h2loader_cli_host_path.h"

#include "h2_windows_platform.h"
#include "h2_windows_serial_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

static h2_windows_platform_t *platform;

h2_pal_result_t h2_h2loader_cli_target_start(void) {
    return h2_windows_platform_create_with_logical_drives(&platform);
}

void h2_h2loader_cli_target_stop(void) {
    (void)h2_windows_platform_destroy(&platform);
}

const h2_pal_serial_host_api_t *h2_h2loader_cli_target_serial(void) {
    return h2_windows_serial_host_api();
}

const h2_pal_ble_host_api_t *h2_h2loader_cli_target_ble(
    const h2_pal_mem_api_t *mem) {
    (void)mem;
    return NULL;
}

const h2_pal_system_event_api_t *h2_h2loader_cli_target_system_event(void) {
    return h2_windows_system_event_api(platform);
}

const h2_pal_mem_api_t *h2_h2loader_cli_target_mem(void) {
    return h2_windows_mem_api(platform);
}

const h2_pal_time_api_t *h2_h2loader_cli_target_time(void) {
    return h2_windows_time_api(platform);
}

const h2_pal_task_api_t *h2_h2loader_cli_target_task(void) {
    return h2_windows_task_api(platform);
}

const h2_pal_sync_api_t *h2_h2loader_cli_target_sync(void) {
    return h2_windows_sync_api(platform);
}

const h2_pal_queue_api_t *h2_h2loader_cli_target_queue(void) {
    return h2_windows_queue_api(platform);
}

const h2_pal_log_api_t *h2_h2loader_cli_target_log(void) {
    return h2_windows_log_api(platform);
}

const h2_pal_fs_api_t *h2_h2loader_cli_target_fs(void) {
    return h2_windows_fs_api(platform);
}

const h2_pal_net_api_t *h2_h2loader_cli_target_net(void) {
    return h2_windows_net_api(platform);
}

static char *windows_working_directory(void) {
    DWORD capacity = GetEnvironmentVariableW(L"BUILD_WORKING_DIRECTORY", NULL, 0u);
    int from_environment = capacity != 0u;
    if (!from_environment) capacity = GetCurrentDirectoryW(0u, NULL);
    if (capacity == 0u) return NULL;
    wchar_t *wide = malloc((size_t)capacity * sizeof(*wide));
    if (wide == NULL) return NULL;
    DWORD copied = from_environment
                       ? GetEnvironmentVariableW(L"BUILD_WORKING_DIRECTORY", wide,
                                                 capacity)
                       : GetCurrentDirectoryW(capacity, wide);
    char *utf8 = NULL;
    if (copied != 0u && copied < capacity) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                       -1, NULL, 0, NULL, NULL);
        if (bytes > 0) {
            utf8 = malloc((size_t)bytes);
            if (utf8 != NULL &&
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                    utf8, bytes, NULL, NULL) != bytes) {
                free(utf8);
                utf8 = NULL;
            }
        }
    }
    free(wide);
    return utf8;
}

int h2_h2loader_cli_target_resolve_path(const char *path, char *out, size_t out_size) {
    char source_storage[26][4];
    char target_storage[26][3];
    const char *sources[26];
    const char *targets[26];
    for (size_t i = 0u; i < 26u; ++i) {
        source_storage[i][0] = (char)('A' + (int)i);
        source_storage[i][1] = ':';
        source_storage[i][2] = '\\';
        source_storage[i][3] = '\0';
        target_storage[i][0] = '/';
        target_storage[i][1] = (char)('a' + (int)i);
        target_storage[i][2] = '\0';
        sources[i] = source_storage[i];
        targets[i] = target_storage[i];
    }
    /* The provider owns the accessible-drive inventory. A rewritten path
     * for an unmounted drive still fails through the PAL namespace. */
    char *base = windows_working_directory();
    int result = h2_h2loader_cli_host_path_resolve(
        base, sources, targets, 26u, path, out, out_size);
    free(base);
    return result;
}
