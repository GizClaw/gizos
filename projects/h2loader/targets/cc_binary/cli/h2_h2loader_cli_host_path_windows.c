#include "h2_h2loader_cli_host_path.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int absolute_dos_path(const wchar_t *path, size_t length) {
    return length >= 3u &&
           ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

static wchar_t *wide_path(const char *path) {
    if (path == NULL || path[0] == '\0')
        return NULL;
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (wide == NULL || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                            -1, wide, count) != count) {
        free(wide);
        return NULL;
    }
    size_t length = wcslen(wide);
    if (length >= 7u && wide[0] == L'\\' && wide[1] == L'\\' &&
        wide[2] == L'?' && wide[3] == L'\\' &&
        absolute_dos_path(wide + 4u, length - 4u)) {
        memmove(wide, wide + 4u, (length - 4u + 1u) * sizeof(*wide));
    }
    return wide;
}

static wchar_t *full_dos_path(const wchar_t *path) {
    DWORD capacity = GetFullPathNameW(path, 0u, NULL, NULL);
    if (capacity == 0u)
        return NULL;
    wchar_t *full = malloc((size_t)capacity * sizeof(*full));
    if (full == NULL)
        return NULL;
    DWORD copied = GetFullPathNameW(path, capacity, full, NULL);
    if (copied == 0u || copied >= capacity ||
        !absolute_dos_path(full, (size_t)copied)) {
        free(full);
        return NULL;
    }
    return full;
}

static wchar_t *resolve_input(const char *base_dir, const char *path) {
    /* A slash-leading argument is already in the portable PAL namespace. */
    if (path == NULL || path[0] == '/')
        return NULL;
    wchar_t *wide = wide_path(path);
    if (wide == NULL)
        return NULL;
    size_t length = wcslen(wide);
    if (absolute_dos_path(wide, length)) {
        wchar_t *full = full_dos_path(wide);
        free(wide);
        return full;
    }
    /* UNC/device paths and drive-relative paths do not have a DOS-root
     * mapping. Do not resolve them using another drive's implicit cwd. */
    if (wide[0] == L'\\' || (length >= 2u && wide[1] == L':')) {
        free(wide);
        return NULL;
    }
    wchar_t *base = wide_path(base_dir);
    if (base == NULL || !absolute_dos_path(base, wcslen(base))) {
        free(base);
        free(wide);
        return NULL;
    }
    size_t base_length = wcslen(base);
    size_t max_characters = SIZE_MAX / sizeof(*wide);
    if (length > max_characters - 2u ||
        base_length > max_characters - length - 2u) {
        free(base);
        free(wide);
        return NULL;
    }
    wchar_t *joined = malloc((base_length + length + 2u) * sizeof(*joined));
    wchar_t *full = NULL;
    if (joined != NULL) {
        memcpy(joined, base, base_length * sizeof(*joined));
        joined[base_length] = L'\\';
        memcpy(joined + base_length + 1u, wide,
               (length + 1u) * sizeof(*joined));
        full = full_dos_path(joined);
    }
    free(joined);
    free(base);
    free(wide);
    return full;
}

int h2_h2loader_cli_host_path_resolve(const char *base_dir,
                                      const char *const *sources,
                                      const char *const *targets,
                                      size_t mount_count, const char *path,
                                      char *out, size_t out_size) {
    if (out == NULL || out_size == 0u)
        return 0;
    out[0] = '\0';
    if (sources == NULL || targets == NULL || mount_count == 0u)
        return 0;
    wchar_t *full = resolve_input(base_dir, path);
    if (full == NULL)
        return 0;
    int result = 0;
    for (size_t index = 0u; index < mount_count; ++index) {
        wchar_t *source_input = wide_path(sources[index]);
        wchar_t *source =
            source_input != NULL &&
                    absolute_dos_path(source_input, wcslen(source_input))
                ? full_dos_path(source_input)
                : NULL;
        free(source_input);
        if (source == NULL || targets[index] == NULL) {
            free(source);
            continue;
        }
        size_t source_length = wcslen(source);
        while (source_length > 3u && source[source_length - 1u] == L'\\') {
            source[--source_length] = L'\0';
        }
        size_t full_length = wcslen(full);
        int matches =
            full_length >= source_length &&
            _wcsnicmp(source, full, source_length) == 0 &&
            (source[source_length - 1u] == L'\\' ||
             full[source_length] == L'\0' || full[source_length] == L'\\');
        free(source);
        if (!matches)
            continue;
        const wchar_t *relative = full + source_length;
        if (*relative == L'\\')
            ++relative;
        int relative_bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, relative, -1, NULL, 0, NULL, NULL);
        size_t target_length = strlen(targets[index]);
        size_t separator = relative[0] == L'\0' ? 0u : 1u;
        if (relative_bytes <= 0 || target_length >= out_size ||
            separator >= out_size - target_length ||
            (size_t)relative_bytes > out_size - target_length - separator) {
            break;
        }
        memcpy(out, targets[index], target_length);
        if (separator != 0u)
            out[target_length] = '/';
        char *suffix = out + target_length + separator;
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative, -1,
                                suffix, relative_bytes, NULL,
                                NULL) != relative_bytes) {
            out[0] = '\0';
            break;
        }
        for (char *cursor = suffix; *cursor != '\0'; ++cursor) {
            if (*cursor == '\\')
                *cursor = '/';
        }
        result = 1;
        break;
    }
    free(full);
    return result;
}
