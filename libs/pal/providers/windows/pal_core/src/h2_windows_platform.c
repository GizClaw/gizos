#include "h2_windows_internal.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void *h2_windows_heap_alloc(size_t size) {
    SIZE_T requested = size == 0u ? 1u : (SIZE_T)size;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, requested);
}

void *h2_windows_heap_realloc(void *memory, size_t size) {
    if (memory == NULL) {
        return h2_windows_heap_alloc(size);
    }
    SIZE_T requested = size == 0u ? 1u : (SIZE_T)size;
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, memory, requested);
}

void h2_windows_heap_free(void *memory) {
    if (memory != NULL) {
        (void)HeapFree(GetProcessHeap(), 0u, memory);
    }
}

void h2_windows_object_acquire(h2_windows_platform_t *platform) {
    (void)InterlockedIncrement(&platform->live_objects);
}

void h2_windows_object_release(h2_windows_platform_t *platform) {
    (void)InterlockedDecrement(&platform->live_objects);
    WakeAllConditionVariable(&platform->idle);
}

h2_pal_result_t h2_windows_error_from_win32(DWORD error) {
    switch (error) {
        case ERROR_SUCCESS:
            return H2_PAL_OK;
        case ERROR_INVALID_PARAMETER:
        case ERROR_BAD_ARGUMENTS:
            return H2_PAL_ERR_INVALID_ARG;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return H2_PAL_ERR_NO_MEMORY;
        case ERROR_TIMEOUT:
            return H2_PAL_ERR_TIMEOUT;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_FOUND:
            return H2_PAL_ERR_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return H2_PAL_ERR_UNAVAILABLE;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return H2_PAL_ERR_NO_SPACE;
        case ERROR_BUSY:
        case ERROR_PIPE_BUSY:
            return H2_PAL_ERR_BUSY;
        default:
            return H2_PAL_ERR_IO;
    }
}

h2_pal_result_t h2_windows_error_from_wsa(int error) {
    switch (error) {
        case 0:
            return H2_PAL_OK;
        case WSAEINVAL:
        case WSAEAFNOSUPPORT:
        case WSAEDESTADDRREQ:
            return H2_PAL_ERR_INVALID_ARG;
        case WSAEWOULDBLOCK:
        case WSAEINPROGRESS:
        case WSAEALREADY:
            return H2_PAL_ERR_WOULD_BLOCK;
        case WSAETIMEDOUT:
            return H2_PAL_ERR_TIMEOUT;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENOTCONN:
        case WSAESHUTDOWN:
            return H2_PAL_ERR_CLOSED;
        case WSAHOST_NOT_FOUND:
        case WSANO_DATA:
            return H2_PAL_ERR_NOT_FOUND;
        case WSAENOBUFS:
        case WSAEMFILE:
            return H2_PAL_ERR_NO_SPACE;
        case WSAEADDRINUSE:
            return H2_PAL_ERR_BUSY;
        default:
            return H2_PAL_ERR_IO;
    }
}

wchar_t *h2_windows_utf8_to_wide(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                    NULL, 0);
    if (count <= 0) {
        return NULL;
    }
    wchar_t *wide = h2_windows_heap_alloc((size_t)count * sizeof(*wide));
    if (wide == NULL ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            count) != count) {
        h2_windows_heap_free(wide);
        return NULL;
    }
    return wide;
}

char *h2_windows_wide_to_utf8(const wchar_t *text) {
    if (text == NULL) {
        return NULL;
    }
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                    NULL, 0, NULL, NULL);
    if (count <= 0) {
        return NULL;
    }
    char *utf8 = h2_windows_heap_alloc((size_t)count);
    if (utf8 == NULL ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8,
                            count, NULL, NULL) != count) {
        h2_windows_heap_free(utf8);
        return NULL;
    }
    return utf8;
}

static void windows_free_mounts(h2_windows_platform_t *platform) {
    for (size_t index = 0u; index < platform->mount_count; ++index) {
        h2_windows_heap_free(platform->mounts[index].source);
        h2_windows_heap_free(platform->mounts[index].target);
    }
    h2_windows_heap_free(platform->mounts);
    platform->mounts = NULL;
    platform->mount_count = 0u;
}

static int windows_mount_target_valid(const char *target) {
    if (target == NULL || target[0] != '/' || target[1] == '\0' ||
        target[strlen(target) - 1u] == '/') {
        return 0;
    }
    const char *component = target + 1;
    while (*component != '\0') {
        const char *slash = strchr(component, '/');
        size_t len = slash == NULL ? strlen(component)
                                   : (size_t)(slash - component);
        if (len == 0u || (len == 1u && component[0] == '.') ||
            (len == 2u && component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        for (size_t index = 0u; index < len; ++index) {
            unsigned char value = (unsigned char)component[index];
            if (value < 0x20u || value == '\\' || value == ':' ||
                value == '*' || value == '?' || value == '"' ||
                value == '<' || value == '>' || value == '|') {
                return 0;
            }
        }
        if (slash == NULL) {
            break;
        }
        component = slash + 1;
    }
    return 1;
}

static h2_pal_result_t windows_copy_mounts(
    h2_windows_platform_t *platform,
    const h2_windows_platform_config_t *config) {
    if (config->fs_mount_count == 0u) {
        return config->fs_sources == NULL && config->fs_targets == NULL
                   ? H2_PAL_OK
                   : H2_PAL_ERR_INVALID_ARG;
    }
    if (config->fs_sources == NULL || config->fs_targets == NULL ||
        config->fs_mount_count > SIZE_MAX / sizeof(*platform->mounts)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->mounts = h2_windows_heap_alloc(
        config->fs_mount_count * sizeof(*platform->mounts));
    if (platform->mounts == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    platform->mount_count = config->fs_mount_count;
    for (size_t index = 0u; index < config->fs_mount_count; ++index) {
        const char *source = config->fs_sources[index];
        const char *target = config->fs_targets[index];
        if (source == NULL || !windows_mount_target_valid(target)) {
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        wchar_t *source_input = h2_windows_utf8_to_wide(source);
        platform->mounts[index].target = h2_windows_utf8_to_wide(target);
        if (source_input == NULL ||
            platform->mounts[index].target == NULL) {
            h2_windows_heap_free(source_input);
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        DWORD source_count = GetFullPathNameW(source_input, 0u, NULL, NULL);
        if (source_count == 0u || wcslen(source_input) < 3u ||
            source_input[1] != L':' ||
            source_input[2] != L'\\') {
            h2_windows_heap_free(source_input);
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        wchar_t *source_full = h2_windows_heap_alloc(
            (size_t)source_count * sizeof(wchar_t));
        if (source_full == NULL ||
            GetFullPathNameW(source_input, source_count, source_full, NULL) ==
                0u) {
            h2_windows_heap_free(source_input);
            h2_windows_heap_free(source_full);
            windows_free_mounts(platform);
            return H2_PAL_ERR_NO_MEMORY;
        }
        h2_windows_heap_free(source_input);
        if (source_full[1] != L':' || source_full[2] != L'\\') {
            h2_windows_heap_free(source_full);
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        HANDLE source_handle = CreateFileW(
            source_full, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        h2_windows_heap_free(source_full);
        if (source_handle == INVALID_HANDLE_VALUE) {
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        DWORD final_count = GetFinalPathNameByHandleW(
            source_handle, NULL, 0u, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        platform->mounts[index].source = h2_windows_heap_alloc(
            ((size_t)final_count + 1u) * sizeof(wchar_t));
        DWORD copied_count = platform->mounts[index].source == NULL
                                 ? 0u
                                 : GetFinalPathNameByHandleW(
                                       source_handle,
                                       platform->mounts[index].source,
                                       final_count + 1u,
                                       FILE_NAME_NORMALIZED |
                                           VOLUME_NAME_DOS);
        if (final_count == 0u || platform->mounts[index].source == NULL ||
            copied_count == 0u || copied_count > final_count) {
            (void)CloseHandle(source_handle);
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        (void)CloseHandle(source_handle);
        platform->mounts[index].source_len = wcslen(
            platform->mounts[index].source);
        while (platform->mounts[index].source_len > 3u &&
               platform->mounts[index]
                       .source[platform->mounts[index].source_len - 1u] ==
                   L'\\') {
            platform->mounts[index]
                .source[--platform->mounts[index].source_len] = L'\0';
        }
        platform->mounts[index].target_len = wcslen(
            platform->mounts[index].target);
        DWORD attributes = GetFileAttributesW(platform->mounts[index].source);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            windows_free_mounts(platform);
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (size_t previous = 0u; previous < index; ++previous) {
            size_t shorter = platform->mounts[previous].target_len <
                                     platform->mounts[index].target_len
                                 ? platform->mounts[previous].target_len
                                 : platform->mounts[index].target_len;
            int prefix_equal = _wcsnicmp(
                                   platform->mounts[previous].target,
                                   platform->mounts[index].target, shorter) ==
                               0;
            int previous_contains =
                platform->mounts[previous].target_len == shorter &&
                (platform->mounts[index].target_len == shorter ||
                 platform->mounts[index].target[shorter] == L'/');
            int current_contains =
                platform->mounts[index].target_len == shorter &&
                (platform->mounts[previous].target_len == shorter ||
                 platform->mounts[previous].target[shorter] == L'/');
            if (prefix_equal && (previous_contains || current_contains)) {
                windows_free_mounts(platform);
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_windows_platform_create(
    const h2_windows_platform_config_t *config,
    h2_windows_platform_t **out_platform) {
    if (config == NULL || out_platform == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_platform = NULL;
    h2_windows_platform_t *platform = h2_windows_heap_alloc(sizeof(*platform));
    if (platform == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    InitializeCriticalSection(&platform->lock);
    InitializeConditionVariable(&platform->idle);
    for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
        InitializeCriticalSection(&platform->sockets[index].lock);
        platform->sockets[index].socket = INVALID_SOCKET;
    }
    if (!QueryPerformanceFrequency(&platform->qpc_frequency) ||
        platform->qpc_frequency.QuadPart <= 0) {
        for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
            DeleteCriticalSection(&platform->sockets[index].lock);
        }
        DeleteCriticalSection(&platform->lock);
        h2_windows_heap_free(platform);
        return H2_PAL_ERR_IO;
    }
    h2_pal_result_t result = windows_copy_mounts(platform, config);
    if (result != H2_PAL_OK) {
        for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
            DeleteCriticalSection(&platform->sockets[index].lock);
        }
        DeleteCriticalSection(&platform->lock);
        h2_windows_heap_free(platform);
        return result;
    }
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        windows_free_mounts(platform);
        for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
            DeleteCriticalSection(&platform->sockets[index].lock);
        }
        DeleteCriticalSection(&platform->lock);
        h2_windows_heap_free(platform);
        return H2_PAL_ERR_IO;
    }
    platform->mem_api = (h2_pal_mem_api_t){platform, &h2_windows_mem_vtable};
    platform->log_api = (h2_pal_log_api_t){platform, &h2_windows_log_vtable};
    platform->time_api = (h2_pal_time_api_t){platform, &h2_windows_time_vtable};
    platform->timer_api = (h2_pal_timer_api_t){platform, &h2_windows_timer_vtable};
    platform->task_api = (h2_pal_task_api_t){platform, &h2_windows_task_vtable};
    platform->queue_api = (h2_pal_queue_api_t){platform, &h2_windows_queue_vtable};
    platform->sync_api = (h2_pal_sync_api_t){platform, &h2_windows_sync_vtable};
    platform->fs_api = (h2_pal_fs_api_t){platform, &h2_windows_fs_vtable};
    platform->net_api = (h2_pal_net_api_t){platform, &h2_windows_net_vtable};
    platform->netif_api = (h2_pal_netif_api_t){platform, &h2_windows_netif_vtable};
    platform->system_event_api = (h2_pal_system_event_api_t){
        platform, &h2_windows_system_event_vtable};
    *out_platform = platform;
    return H2_PAL_OK;
}

h2_pal_result_t h2_windows_platform_destroy(
    h2_windows_platform_t **platform_ptr) {
    if (platform_ptr == NULL || *platform_ptr == NULL) {
        return H2_PAL_OK;
    }
    h2_windows_platform_t *platform = *platform_ptr;
    if (InterlockedCompareExchange(&platform->live_allocations, 0, 0) != 0 ||
        InterlockedCompareExchange(&platform->live_objects, 0, 0) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    (void)InterlockedExchange(&platform->shutting_down, 1);
    h2_pal_result_t result = h2_windows_system_event_shutdown(platform);
    if (result == H2_PAL_OK) {
        result = h2_windows_net_shutdown(platform);
    }
    if (result != H2_PAL_OK) {
        (void)InterlockedExchange(&platform->shutting_down, 0);
        return result;
    }
    (void)WSACleanup();
    windows_free_mounts(platform);
    for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
        DeleteCriticalSection(&platform->sockets[index].lock);
    }
    DeleteCriticalSection(&platform->lock);
    h2_windows_heap_free(platform);
    *platform_ptr = NULL;
    return H2_PAL_OK;
}

#define H2_WINDOWS_ACCESSOR(name, field, type)                                 \
    const type *name(h2_windows_platform_t *platform) {                        \
        return platform == NULL ? NULL : &platform->field;                     \
    }

H2_WINDOWS_ACCESSOR(h2_windows_mem_api, mem_api, h2_pal_mem_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_log_api, log_api, h2_pal_log_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_time_api, time_api, h2_pal_time_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_timer_api, timer_api, h2_pal_timer_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_task_api, task_api, h2_pal_task_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_queue_api, queue_api, h2_pal_queue_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_sync_api, sync_api, h2_pal_sync_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_fs_api, fs_api, h2_pal_fs_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_net_api, net_api, h2_pal_net_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_netif_api, netif_api, h2_pal_netif_api_t)
H2_WINDOWS_ACCESSOR(h2_windows_system_event_api, system_event_api,
                    h2_pal_system_event_api_t)

#undef H2_WINDOWS_ACCESSOR
