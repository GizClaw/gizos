#include "h2_windows_internal.h"

#include <string.h>
#include <wchar.h>

struct h2_pal_fs_file {
    h2_windows_platform_t *platform;
    HANDLE native;
};

static int windows_ascii_equal(const char *value, size_t value_len,
                               const char *expected) {
    size_t expected_len = strlen(expected);
    return value_len == expected_len &&
           _strnicmp(value, expected, value_len) == 0;
}

static int windows_component_is_device(const char *component, size_t len) {
    size_t stem_len = 0u;
    while (stem_len < len && component[stem_len] != '.') {
        ++stem_len;
    }
    if (windows_ascii_equal(component, stem_len, "CON") ||
        windows_ascii_equal(component, stem_len, "PRN") ||
        windows_ascii_equal(component, stem_len, "AUX") ||
        windows_ascii_equal(component, stem_len, "NUL") ||
        windows_ascii_equal(component, stem_len, "CONIN$") ||
        windows_ascii_equal(component, stem_len, "CONOUT$")) {
        return 1;
    }
    return stem_len == 4u && component[3] >= '1' && component[3] <= '9' &&
           (_strnicmp(component, "COM", 3u) == 0 ||
            _strnicmp(component, "LPT", 3u) == 0);
}

static int windows_portable_path_valid(const char *path) {
    if (path == NULL || path[0] != '/' || path[1] == '\0') {
        return 0;
    }
    size_t path_len = strlen(path);
    if (path[path_len - 1u] == '/') {
        return 0;
    }
    const char *component = path + 1;
    while (*component != '\0') {
        const char *slash = strchr(component, '/');
        size_t len = slash == NULL ? strlen(component)
                                   : (size_t)(slash - component);
        if (len == 0u || (len == 1u && component[0] == '.') ||
            (len == 2u && component[0] == '.' && component[1] == '.') ||
            component[len - 1u] == '.' || component[len - 1u] == ' ' ||
            windows_component_is_device(component, len)) {
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

static int windows_opened_path_valid(const h2_windows_mount_t *mount,
                                     HANDLE native) {
    FILE_ATTRIBUTE_TAG_INFO tag_info;
    if (!GetFileInformationByHandleEx(native, FileAttributeTagInfo, &tag_info,
                                      sizeof(tag_info))) {
        return h2_windows_error_from_win32(GetLastError());
    }
    if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DWORD count = GetFinalPathNameByHandleW(
        native, NULL, 0u, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (count == 0u) {
        return h2_windows_error_from_win32(GetLastError());
    }
    wchar_t *final_path = h2_windows_heap_alloc(
        ((size_t)count + 1u) * sizeof(*final_path));
    if (final_path == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    DWORD copied = GetFinalPathNameByHandleW(
        native, final_path, count + 1u,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    int result = H2_PAL_ERR_INVALID_ARG;
    if (copied >= mount->source_len && copied <= count &&
        _wcsnicmp(mount->source, final_path, mount->source_len) == 0 &&
        (final_path[mount->source_len] == L'\0' ||
         final_path[mount->source_len] == L'\\')) {
        result = H2_PAL_OK;
    }
    h2_windows_heap_free(final_path);
    return result;
}

static int windows_mount_contains(const h2_windows_mount_t *mount,
                                  const wchar_t *path,
                                  const wchar_t **out_relative) {
    if (_wcsicmp(mount->target, path) == 0) {
        *out_relative = path + mount->target_len;
        return 1;
    }
    if (_wcsnicmp(mount->target, path, mount->target_len) == 0 &&
        path[mount->target_len] == L'/') {
        *out_relative = path + mount->target_len + 1u;
        return 1;
    }
    return 0;
}

static int windows_reject_reparse_components(const h2_windows_mount_t *mount,
                                              wchar_t *host_path) {
    for (wchar_t *cursor = host_path + mount->source_len;
         *cursor != L'\0'; ++cursor) {
        if (*cursor != L'\\') {
            continue;
        }
        *cursor = L'\0';
        DWORD attributes = GetFileAttributesW(host_path);
        *cursor = L'\\';
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ||
                           error == ERROR_PATH_NOT_FOUND
                       ? H2_PAL_OK
                       : h2_windows_error_from_win32(error);
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    DWORD attributes = GetFileAttributesW(host_path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return h2_windows_error_from_win32(error);
        }
    }
    return H2_PAL_OK;
}

static int windows_resolve_path(h2_windows_platform_t *platform,
                                const char *path,
                                h2_windows_mount_t **out_mount,
                                wchar_t **out_host_path) {
    if (!windows_portable_path_valid(path) || out_mount == NULL ||
        out_host_path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_mount = NULL;
    *out_host_path = NULL;
    wchar_t *wide_path = h2_windows_utf8_to_wide(path);
    if (wide_path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = H2_PAL_ERR_NOT_FOUND;
    for (size_t index = 0u; index < platform->mount_count; ++index) {
        const wchar_t *relative = NULL;
        if (!windows_mount_contains(&platform->mounts[index], wide_path,
                                    &relative)) {
            continue;
        }
        size_t relative_len = wcslen(relative);
        size_t separator_len = relative_len == 0u ? 0u : 1u;
        if (platform->mounts[index].source_len >
            SIZE_MAX - separator_len - relative_len - 1u) {
            result = H2_PAL_ERR_NO_MEMORY;
            break;
        }
        size_t total = platform->mounts[index].source_len + separator_len +
                       relative_len + 1u;
        wchar_t *host_path = h2_windows_heap_alloc(total * sizeof(wchar_t));
        if (host_path == NULL) {
            result = H2_PAL_ERR_NO_MEMORY;
            break;
        }
        memcpy(host_path, platform->mounts[index].source,
               platform->mounts[index].source_len * sizeof(wchar_t));
        size_t offset = platform->mounts[index].source_len;
        if (relative_len != 0u) {
            host_path[offset++] = L'\\';
            memcpy(host_path + offset, relative,
                   relative_len * sizeof(wchar_t));
            for (size_t child = 0u; child < relative_len; ++child) {
                if (host_path[offset + child] == L'/') {
                    host_path[offset + child] = L'\\';
                }
            }
            offset += relative_len;
        }
        host_path[offset] = L'\0';
        result = windows_reject_reparse_components(
            &platform->mounts[index], host_path);
        if (result == H2_PAL_OK) {
            *out_mount = &platform->mounts[index];
            *out_host_path = host_path;
        } else {
            h2_windows_heap_free(host_path);
        }
        break;
    }
    h2_windows_heap_free(wide_path);
    return result;
}

typedef struct windows_path_guard {
    HANDLE *handles;
    size_t count;
} windows_path_guard_t;

static void windows_path_guard_close(windows_path_guard_t *guard) {
    if (guard == NULL) {
        return;
    }
    while (guard->count != 0u) {
        (void)CloseHandle(guard->handles[--guard->count]);
    }
    h2_windows_heap_free(guard->handles);
    guard->handles = NULL;
}

static int windows_open_guarded_directory(const h2_windows_mount_t *mount,
                                          const wchar_t *path,
                                          HANDLE *out_handle) {
    HANDLE handle = CreateFileW(
        path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return h2_windows_error_from_win32(GetLastError());
    }
    int result = windows_opened_path_valid(mount, handle);
    if (result == H2_PAL_OK) {
        FILE_ATTRIBUTE_TAG_INFO info;
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                                          sizeof(info))) {
            result = h2_windows_error_from_win32(GetLastError());
        } else if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            result = H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (result != H2_PAL_OK) {
        (void)CloseHandle(handle);
        return result;
    }
    *out_handle = handle;
    return H2_PAL_OK;
}

/* Retaining every directory without FILE_SHARE_DELETE prevents a concurrent
 * rename/replacement from changing the meaning of the validated path. */
static int windows_guard_path(const h2_windows_mount_t *mount,
                              const wchar_t *host_path, int include_leaf,
                              windows_path_guard_t *out_guard) {
    memset(out_guard, 0, sizeof(*out_guard));
    size_t path_len = wcslen(host_path);
    size_t capacity = path_len - mount->source_len + 1u;
    out_guard->handles =
        h2_windows_heap_alloc(capacity * sizeof(*out_guard->handles));
    wchar_t *candidate = h2_windows_heap_alloc((path_len + 1u) *
                                                sizeof(*candidate));
    if (out_guard->handles == NULL || candidate == NULL) {
        h2_windows_heap_free(candidate);
        windows_path_guard_close(out_guard);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(candidate, host_path, (path_len + 1u) * sizeof(*candidate));
    int result = windows_open_guarded_directory(
        mount, mount->source, &out_guard->handles[out_guard->count]);
    if (result == H2_PAL_OK) {
        ++out_guard->count;
    }
    for (size_t index = mount->source_len + 1u;
         result == H2_PAL_OK && index < path_len; ++index) {
        if (candidate[index] != L'\\') {
            continue;
        }
        candidate[index] = L'\0';
        result = windows_open_guarded_directory(
            mount, candidate, &out_guard->handles[out_guard->count]);
        candidate[index] = L'\\';
        if (result == H2_PAL_OK) {
            ++out_guard->count;
        }
    }
    if (result == H2_PAL_OK && include_leaf &&
        path_len != mount->source_len) {
        result = windows_open_guarded_directory(
            mount, host_path, &out_guard->handles[out_guard->count]);
        if (result == H2_PAL_OK) {
            ++out_guard->count;
        }
    }
    h2_windows_heap_free(candidate);
    if (result != H2_PAL_OK) {
        windows_path_guard_close(out_guard);
    }
    return result;
}

static int windows_open_mutation_target(const h2_windows_mount_t *mount,
                                        const wchar_t *path,
                                        HANDLE *out_handle, int *out_is_dir) {
    HANDLE handle = CreateFileW(
        path, DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return h2_windows_error_from_win32(GetLastError());
    }
    int result = windows_opened_path_valid(mount, handle);
    FILE_ATTRIBUTE_TAG_INFO info = {0};
    if (result == H2_PAL_OK &&
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                                      sizeof(info))) {
        result = h2_windows_error_from_win32(GetLastError());
    }
    if (result != H2_PAL_OK) {
        (void)CloseHandle(handle);
        return result;
    }
    *out_is_dir =
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    *out_handle = handle;
    return H2_PAL_OK;
}

static int windows_delete_opened(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo,
                                      &disposition, sizeof(disposition))
               ? H2_PAL_OK
               : h2_windows_error_from_win32(GetLastError());
}

static int windows_fs_mkdir(void *user, const char *path) {
    h2_windows_mount_t *mount = NULL;
    wchar_t *host_path = NULL;
    int result = windows_resolve_path(user, path, &mount, &host_path);
    if (result != H2_PAL_OK) {
        return result;
    }
    windows_path_guard_t guard;
    result = windows_guard_path(mount, host_path, 0, &guard);
    if (result != H2_PAL_OK) {
        h2_windows_heap_free(host_path);
        return result;
    }
    if (!CreateDirectoryW(host_path, NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            result = h2_windows_error_from_win32(error);
        } else {
            result = H2_PAL_OK;
        }
    }
    if (result == H2_PAL_OK) {
        HANDLE created = INVALID_HANDLE_VALUE;
        result = windows_open_guarded_directory(mount, host_path, &created);
        if (created != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(created);
        }
    }
    windows_path_guard_close(&guard);
    h2_windows_heap_free(host_path);
    return result;
}

static int windows_fs_open(void *user, const char *path,
                           h2_pal_fs_open_mode_t mode,
                           h2_pal_fs_file_t **out_file) {
    h2_windows_platform_t *platform = user;
    if (out_file == NULL ||
        (mode != H2_PAL_FS_OPEN_READ &&
         mode != H2_PAL_FS_OPEN_WRITE_TRUNCATE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    h2_windows_mount_t *mount = NULL;
    wchar_t *host_path = NULL;
    int result = windows_resolve_path(platform, path, &mount, &host_path);
    if (result != H2_PAL_OK) {
        return result;
    }
    DWORD access = mode == H2_PAL_FS_OPEN_READ ? GENERIC_READ : GENERIC_WRITE;
    DWORD creation = mode == H2_PAL_FS_OPEN_READ ? OPEN_EXISTING : OPEN_ALWAYS;
    HANDLE native = CreateFileW(host_path, access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, creation,
                                FILE_ATTRIBUTE_NORMAL |
                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    if (native == INVALID_HANDLE_VALUE) {
        h2_windows_heap_free(host_path);
        return h2_windows_error_from_win32(GetLastError());
    }
    result = windows_opened_path_valid(mount, native);
    if (result == H2_PAL_OK && mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(native, zero, NULL, FILE_BEGIN) ||
            !SetEndOfFile(native)) {
            result = h2_windows_error_from_win32(GetLastError());
        }
    }
    h2_windows_heap_free(host_path);
    if (result != H2_PAL_OK) {
        (void)CloseHandle(native);
        return result;
    }
    h2_pal_fs_file_t *file = h2_windows_heap_alloc(sizeof(*file));
    if (file == NULL) {
        (void)CloseHandle(native);
        return H2_PAL_ERR_NO_MEMORY;
    }
    file->platform = platform;
    file->native = native;
    h2_windows_object_acquire(platform);
    *out_file = file;
    return H2_PAL_OK;
}

static int windows_fs_read(void *user, h2_pal_fs_file_t *file, void *data,
                           size_t len, size_t *out_read) {
    if (file == NULL || file->platform != user || out_read == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    while (*out_read < len) {
        size_t remaining = len - *out_read;
        DWORD chunk = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
        DWORD received = 0u;
        if (!ReadFile(file->native, (uint8_t *)data + *out_read, chunk,
                      &received, NULL)) {
            return h2_windows_error_from_win32(GetLastError());
        }
        *out_read += received;
        if (received < chunk) {
            break;
        }
    }
    return H2_PAL_OK;
}

static int windows_fs_seek(void *user, h2_pal_fs_file_t *file,
                           uint64_t position) {
    if (file == NULL || file->platform != user || position > INT64_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    LARGE_INTEGER distance;
    distance.QuadPart = (LONGLONG)position;
    return SetFilePointerEx(file->native, distance, NULL, FILE_BEGIN)
               ? H2_PAL_OK
               : h2_windows_error_from_win32(GetLastError());
}

static int windows_fs_write(void *user, h2_pal_fs_file_t *file,
                            const void *data, size_t len,
                            size_t *out_written) {
    if (file == NULL || file->platform != user || out_written == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    while (*out_written < len) {
        size_t remaining = len - *out_written;
        DWORD chunk = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
        DWORD written = 0u;
        if (!WriteFile(file->native, (const uint8_t *)data + *out_written,
                       chunk, &written, NULL)) {
            return h2_windows_error_from_win32(GetLastError());
        }
        *out_written += written;
        if (written == 0u) {
            return H2_PAL_ERR_IO;
        }
    }
    return H2_PAL_OK;
}

static int windows_fs_sync(void *user, h2_pal_fs_file_t *file) {
    if (file == NULL || file->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return FlushFileBuffers(file->native)
               ? H2_PAL_OK
               : h2_windows_error_from_win32(GetLastError());
}

static int windows_fs_close(void *user, h2_pal_fs_file_t *file) {
    h2_windows_platform_t *platform = user;
    if (file == NULL || file->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = CloseHandle(file->native)
                     ? H2_PAL_OK
                     : h2_windows_error_from_win32(GetLastError());
    if (result == H2_PAL_OK) {
        h2_windows_heap_free(file);
        h2_windows_object_release(platform);
    }
    return result;
}

static int windows_fs_stat(void *user, const char *path,
                           h2_pal_fs_stat_t *out_stat) {
    if (out_stat == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    h2_windows_mount_t *mount = NULL;
    wchar_t *host_path = NULL;
    int result = windows_resolve_path(user, path, &mount, &host_path);
    (void)mount;
    if (result != H2_PAL_OK) {
        return result;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(host_path, GetFileExInfoStandard, &attributes)) {
        result = h2_windows_error_from_win32(GetLastError());
    } else {
        ULARGE_INTEGER size;
        size.LowPart = attributes.nFileSizeLow;
        size.HighPart = attributes.nFileSizeHigh;
        out_stat->size = size.QuadPart;
        out_stat->is_dir =
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    }
    h2_windows_heap_free(host_path);
    return result;
}

static int windows_clear_directory(const h2_windows_mount_t *mount,
                                   const wchar_t *path) {
    windows_path_guard_t guard;
    int result = windows_guard_path(mount, path, 1, &guard);
    if (result != H2_PAL_OK) {
        return result;
    }
    size_t path_len = wcslen(path);
    if (path_len > SIZE_MAX - 3u) {
        windows_path_guard_close(&guard);
        return H2_PAL_ERR_NO_MEMORY;
    }
    wchar_t *pattern = h2_windows_heap_alloc(
        (path_len + 3u) * sizeof(wchar_t));
    if (pattern == NULL) {
        windows_path_guard_close(&guard);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(pattern, path, path_len * sizeof(wchar_t));
    pattern[path_len] = L'\\';
    pattern[path_len + 1u] = L'*';
    pattern[path_len + 2u] = L'\0';
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    h2_windows_heap_free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        windows_path_guard_close(&guard);
        return error == ERROR_FILE_NOT_FOUND
                   ? H2_PAL_OK
                   : h2_windows_error_from_win32(error);
    }
    result = H2_PAL_OK;
    do {
        if (wcscmp(entry.cFileName, L".") == 0 ||
            wcscmp(entry.cFileName, L"..") == 0) {
            continue;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            result = H2_PAL_ERR_INVALID_ARG;
            break;
        }
        size_t name_len = wcslen(entry.cFileName);
        if (path_len > SIZE_MAX - name_len - 2u) {
            result = H2_PAL_ERR_NO_MEMORY;
            break;
        }
        wchar_t *child = h2_windows_heap_alloc(
            (path_len + name_len + 2u) * sizeof(wchar_t));
        if (child == NULL) {
            result = H2_PAL_ERR_NO_MEMORY;
            break;
        }
        memcpy(child, path, path_len * sizeof(wchar_t));
        child[path_len] = L'\\';
        memcpy(child + path_len + 1u, entry.cFileName,
               (name_len + 1u) * sizeof(wchar_t));
        HANDLE child_handle = INVALID_HANDLE_VALUE;
        int child_is_dir = 0;
        result = windows_open_mutation_target(
            mount, child, &child_handle, &child_is_dir);
        if (result == H2_PAL_OK && child_is_dir) {
            (void)CloseHandle(child_handle);
            child_handle = INVALID_HANDLE_VALUE;
            result = windows_clear_directory(mount, child);
            if (result == H2_PAL_OK) {
                result = windows_open_mutation_target(
                    mount, child, &child_handle, &child_is_dir);
            }
        }
        if (result == H2_PAL_OK) {
            result = windows_delete_opened(child_handle);
        }
        if (child_handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(child_handle);
        }
        h2_windows_heap_free(child);
        if (result != H2_PAL_OK) {
            break;
        }
    } while (FindNextFileW(find, &entry));
    if (result == H2_PAL_OK && GetLastError() != ERROR_NO_MORE_FILES) {
        result = h2_windows_error_from_win32(GetLastError());
    }
    (void)FindClose(find);
    windows_path_guard_close(&guard);
    return result;
}

static int windows_fs_clear(void *user, const char *path) {
    h2_windows_mount_t *mount = NULL;
    wchar_t *host_path = NULL;
    int result = windows_resolve_path(user, path, &mount, &host_path);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = windows_clear_directory(mount, host_path);
    h2_windows_heap_free(host_path);
    return result;
}

static int windows_fs_remove(void *user, const char *path) {
    h2_windows_mount_t *mount = NULL;
    wchar_t *host_path = NULL;
    int result = windows_resolve_path(user, path, &mount, &host_path);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (_wcsicmp(host_path, mount->source) == 0) {
        h2_windows_heap_free(host_path);
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_path_guard_t guard;
    result = windows_guard_path(mount, host_path, 0, &guard);
    HANDLE target = INVALID_HANDLE_VALUE;
    int is_dir = 0;
    if (result == H2_PAL_OK) {
        result = windows_open_mutation_target(mount, host_path, &target,
                                              &is_dir);
    }
    if (result == H2_PAL_OK) {
        result = windows_delete_opened(target);
    }
    if (target != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(target);
    }
    windows_path_guard_close(&guard);
    h2_windows_heap_free(host_path);
    return result;
}

static int windows_fs_rename(void *user, const char *old_path,
                             const char *new_path) {
    h2_windows_mount_t *old_mount = NULL;
    h2_windows_mount_t *new_mount = NULL;
    wchar_t *old_host = NULL;
    wchar_t *new_host = NULL;
    int result = windows_resolve_path(user, old_path, &old_mount, &old_host);
    if (result == H2_PAL_OK) {
        result = windows_resolve_path(user, new_path, &new_mount, &new_host);
    }
    if (result == H2_PAL_OK && old_mount != new_mount) {
        result = H2_PAL_ERR_UNSUPPORTED;
    }
    if (result == H2_PAL_OK &&
        (_wcsicmp(old_host, old_mount->source) == 0 ||
         _wcsicmp(new_host, new_mount->source) == 0)) {
        result = H2_PAL_ERR_INVALID_ARG;
    }
    windows_path_guard_t old_guard = {0};
    windows_path_guard_t new_guard = {0};
    HANDLE source = INVALID_HANDLE_VALUE;
    int source_is_dir = 0;
    if (result == H2_PAL_OK) {
        result = windows_guard_path(old_mount, old_host, 0, &old_guard);
    }
    if (result == H2_PAL_OK) {
        result = windows_guard_path(new_mount, new_host, 0, &new_guard);
    }
    if (result == H2_PAL_OK) {
        result = windows_open_mutation_target(old_mount, old_host, &source,
                                              &source_is_dir);
        (void)source_is_dir;
    }
    if (result == H2_PAL_OK) {
        size_t name_len = wcslen(new_host);
        if (name_len > (SIZE_MAX - sizeof(FILE_RENAME_INFO)) /
                           sizeof(wchar_t) ||
            name_len > UINT32_MAX / sizeof(wchar_t)) {
            result = H2_PAL_ERR_NO_MEMORY;
        } else {
            size_t info_size = sizeof(FILE_RENAME_INFO) +
                               name_len * sizeof(wchar_t);
            FILE_RENAME_INFO *info = h2_windows_heap_alloc(info_size);
            if (info == NULL) {
                result = H2_PAL_ERR_NO_MEMORY;
            } else {
                memset(info, 0, info_size);
                info->ReplaceIfExists = TRUE;
                info->FileNameLength = (DWORD)(name_len * sizeof(wchar_t));
                memcpy(info->FileName, new_host,
                       name_len * sizeof(wchar_t));
                if (!SetFileInformationByHandle(source, FileRenameInfo, info,
                                                (DWORD)info_size)) {
                    result = h2_windows_error_from_win32(GetLastError());
                }
                h2_windows_heap_free(info);
            }
        }
    }
    if (source != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(source);
    }
    windows_path_guard_close(&new_guard);
    windows_path_guard_close(&old_guard);
    h2_windows_heap_free(new_host);
    h2_windows_heap_free(old_host);
    return result;
}

const h2_pal_fs_vtable_t h2_windows_fs_vtable = {
    .mkdir = windows_fs_mkdir,
    .open = windows_fs_open,
    .read = windows_fs_read,
    .seek = windows_fs_seek,
    .write = windows_fs_write,
    .sync = windows_fs_sync,
    .close = windows_fs_close,
    .stat = windows_fs_stat,
    .clear = windows_fs_clear,
    .remove = windows_fs_remove,
    .rename = windows_fs_rename,
};
