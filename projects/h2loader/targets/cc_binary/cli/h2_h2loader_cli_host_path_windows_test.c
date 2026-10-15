#include "h2_h2loader_cli_host_path.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int main(void) {
    wchar_t temp[MAX_PATH];
    wchar_t fixture[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temp) != 0u);
    assert(GetTempFileNameW(temp, L"h2w", 0u, fixture) != 0u);
    assert(DeleteFileW(fixture));
    assert(CreateDirectoryW(fixture, NULL));
    char base[MAX_PATH * 4u];
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, fixture, -1, base,
                               sizeof(base), NULL, NULL) > 0);
    assert(base[1] == ':' && base[2] == '\\');
    char drive[] = {base[0], ':', '\\', '\0'};
    const char *sources[] = {drive};
    const char *targets[] = {"/drive"};
    char input[sizeof(base) + 64u];
    char expected[sizeof(input)];
    char output[sizeof(input)];
    int count = snprintf(input, sizeof(input), "%s\\子目录\\固件.bin", base);
    assert(count > 0 && (size_t)count < sizeof(input));
    count = snprintf(expected, sizeof(expected), "/drive/%s/子目录/固件.bin",
                     base + 3u);
    assert(count > 0 && (size_t)count < sizeof(expected));
    for (char *cursor = expected; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\')
            *cursor = '/';
    }
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 1u, input,
                                             output, sizeof(output)) == 1);
    assert(strcmp(output, expected) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "子目录\\固件.bin", output,
                                             sizeof(output)) == 1);
    assert(strcmp(output, expected) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "子目录/./文件/../固件.bin",
                                             output, sizeof(output)) == 1);
    assert(strcmp(output, expected) == 0);
    char extended[sizeof(input) + 4u];
    count = snprintf(extended, sizeof(extended), "\\\\?\\%s", input);
    assert(count > 0 && (size_t)count < sizeof(extended));
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 1u,
                                             extended, output,
                                             sizeof(output)) == 1);
    assert(strcmp(output, expected) == 0);
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 1u, drive,
                                             output, sizeof(output)) == 1);
    assert(strcmp(output, "/drive") == 0);
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 1u, input,
                                             output, 4u) == 0);
    assert(output[0] == '\0');
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "/drive/file.bin", output,
                                             sizeof(output)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "C:file.bin", output,
                                             sizeof(output)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "\\\\server\\share\\file.bin",
                                             output, sizeof(output)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u,
                                             "\\\\.\\COM8", output,
                                             sizeof(output)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(base, sources, targets, 1u, "\xff",
                                             output, sizeof(output)) == 0);
    const char *scoped_sources[] = {base};
    const char *scoped_targets[] = {"/data"};
    assert(h2_h2loader_cli_host_path_resolve(base, scoped_sources,
                                             scoped_targets, 1u, "child.bin",
                                             output, sizeof(output)) == 1);
    assert(strcmp(output, "/data/child.bin") == 0);
    count = snprintf(input, sizeof(input), "%s-sibling\\child.bin", base);
    assert(count > 0 && (size_t)count < sizeof(input));
    assert(h2_h2loader_cli_host_path_resolve(base, scoped_sources,
                                             scoped_targets, 1u, input, output,
                                             sizeof(output)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 1u,
                                             "child.bin", output,
                                             sizeof(output)) == 0);
    assert(RemoveDirectoryW(fixture));
    puts("Windows native host path tests passed");
    return 0;
}
