#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_h2loader_cli_host_path.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Layout under $TEST_TMPDIR (the sandbox may itself sit behind symlinks, so
 * everything is compared against realpath(tmp)):
 *   <tmp>/real/pkg.bin           regular file
 *   <tmp>/link -> <tmp>/real     symlinked directory (like bazel-bin)
 *   <tmp>/work                   "shell cwd" for relative paths
 * The mount maps host <tmp>/real -> PAL /pkg and host <tmp>/work -> PAL /work. */
int main(void) {
    const char *tmp_env = getenv("TEST_TMPDIR");
    char tmp[PATH_MAX], real_dir[PATH_MAX], link_dir[PATH_MAX], work_dir[PATH_MAX];
    char file[PATH_MAX], out[PATH_MAX];
    assert(tmp_env != NULL && realpath(tmp_env, tmp) != NULL);
    snprintf(real_dir, sizeof(real_dir), "%s/real", tmp);
    snprintf(link_dir, sizeof(link_dir), "%s/link", tmp);
    snprintf(work_dir, sizeof(work_dir), "%s/work", tmp);
    snprintf(file, sizeof(file), "%s/pkg.bin", real_dir);
    assert(mkdir(real_dir, 0700) == 0 && mkdir(work_dir, 0700) == 0);
    assert(symlink(real_dir, link_dir) == 0);
    FILE *f = fopen(file, "wb");
    assert(f != NULL && fclose(f) == 0);

    const char *sources[] = {real_dir, work_dir};
    const char *targets[] = {"/pkg", "/work"};

    /* Absolute path through a symlinked directory resolves into the mount. */
    snprintf(out, sizeof(out), "%s/pkg.bin", link_dir);
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        out, out, sizeof(out)) == 1);
    assert(strcmp(out, "/pkg/pkg.bin") == 0);

    /* Relative path is joined to base_dir, then symlink-resolved. */
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "../link/pkg.bin", out, sizeof(out)) == 1);
    assert(strcmp(out, "/pkg/pkg.bin") == 0);

    /* Relative path inside the base dir maps to its own mount. */
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "x.bin", out, sizeof(out)) == 1);
    assert(strcmp(out, "/work/x.bin") == 0);

    /* Output file that does not exist yet: parent resolved, name appended. */
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "../link/new.tar.zlib", out, sizeof(out)) == 1);
    assert(strcmp(out, "/pkg/new.tar.zlib") == 0);

    /* Mount root itself. */
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        link_dir, out, sizeof(out)) == 1);
    assert(strcmp(out, "/pkg") == 0);

    /* Outside every mount, or unresolvable: left to the caller. */
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "/", out, sizeof(out)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "missing-dir/x.bin", out, sizeof(out)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(NULL, sources, targets, 2u,
        "relative.bin", out, sizeof(out)) == 0);
    assert(h2_h2loader_cli_host_path_resolve(work_dir, sources, targets, 2u,
        "", out, sizeof(out)) == 0);

    puts("h2loader cli host path tests passed");
    return 0;
}
