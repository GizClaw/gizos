"""Verify SD mkdir does not turn native errors into success."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c"


class SdDirectoryTest(unittest.TestCase):
    def test_stat_directory_file_and_missing_path(self):
        source = SOURCE.read_text()
        begin = source.index("static int fs_stat(")
        end = source.index("static int fs_clear(", begin)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_PAL_ERR_NOT_FOUND -2
#define H2_PAL_ERR_IO -4
#define F_ATTR_DIR 16
typedef struct { uint64_t size; int is_dir; } h2_pal_fs_stat_t;
typedef struct { int unused; } FILE;
static FILE native;
static int directory, present, opens, closes;
static long long directory_size;
static int translate_path(const char *path, char *mapped) {
    if (!path || path[0] != '/') return H2_PAL_ERR_INVALID_ARG;
    strcpy(mapped, path); return 0;
}
static int fdir_exist(const char *path) { (void)path; return directory; }
static long long flen_dir(const char *path) { (void)path; return directory_size; }
static FILE *fopen(const char *path, const char *mode) {
    (void)path; assert(strcmp(mode, "r") == 0); ++opens;
    return (present || directory) ? &native : NULL;
}
static int fget_attr(FILE *file, int *attr) {
    assert(file == &native); *attr = directory ? F_ATTR_DIR : 0; return 0;
}
static uint32_t flen(FILE *file) { assert(file == &native); return 123; }
static int fclose(FILE *file) { assert(file == &native); ++closes; return 0; }
'''
        main = r'''
int main(void) {
    h2_pal_fs_stat_t st;
    directory = 1; directory_size = -1;
    assert(fs_stat(NULL, "/dl", &st) == 0 && st.is_dir && st.size == 0);
    assert(fs_stat(NULL, "/data/sub", &st) == 0 && st.is_dir && opens == 2);
    directory_size = 456;
    assert(fs_stat(NULL, "/data/sub", &st) == 0 && st.size == 456);
    directory = 0; present = 1;
    assert(fs_stat(NULL, "/data/file", &st) == 0 && !st.is_dir && st.size == 123);
    assert(opens == 4 && closes == 4);
    present = 0;
    assert(fs_stat(NULL, "/data/missing", &st) == H2_PAL_ERR_NOT_FOUND);
    assert(closes == 4);
    assert(fs_stat(NULL, NULL, &st) == H2_PAL_ERR_INVALID_ARG);
    assert(fs_stat(NULL, "/data", NULL) == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-stat-") as directory:
            root = Path(directory)
            unit = root / "stat.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "stat-test"
            subprocess.run(["cc", "-std=c11", str(unit), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_single_level_directory_and_create_failures(self):
        source = SOURCE.read_text()
        begin = source.index("static int directory_status(")
        end = source.index("static int fs_mkdir(", begin)
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_PAL_ERR_IO -4
#define H2_PAL_ERR_INVALID_STATE -7
#define H2_PAL_ERR_NOT_FOUND -8
#define F_ATTR_DIR 16
typedef struct { char path[194]; int attributes; } FILE;
static FILE entries[16];
static int count, creates, closes, traces;
static int fail_create, fail_attr, fail_close, wrong_type, disappear, concurrent;
static char created_paths[16][194];
static FILE *add(const char *path, int attributes) {
    assert(count < 16);
    FILE *file = &entries[count++];
    strcpy(file->path, path); file->attributes = attributes;
    return file;
}
static FILE *find(const char *path) {
    for (int i = 0; i < count; ++i)
        if (strcmp(entries[i].path, path) == 0) return &entries[i];
    return NULL;
}
static FILE *fopen(const char *path, const char *mode) {
    if (strcmp(mode, "r") == 0) return find(path);
    assert(strcmp(mode, "w+") == 0);
    size_t length = strlen(path);
    assert(length > 0 && path[length - 1] == '/');
    char entry[194]; strcpy(entry, path); entry[length - 1] = 0;
    char parent[194]; strcpy(parent, entry);
    char *slash = strrchr(parent, '/'); assert(slash); *slash = 0;
    if (strcmp(parent, "storage/sd0/C") != 0) {
        FILE *dir = find(parent);
        assert(dir && (dir->attributes & F_ATTR_DIR));
    }
    assert(creates < 16);
    strcpy(created_paths[creates++], entry);
    if (fail_create == creates) return NULL;
    FILE *file = add(entry, wrong_type ? 0 : F_ATTR_DIR);
    if (disappear) file->path[0] = 0;
    return concurrent ? NULL : file;
}
static int fget_attr(FILE *file, int *attributes) {
    if (fail_attr) return -1;
    *attributes = file->attributes; return 0;
}
static int fclose(FILE *file) { (void)file; ++closes; return fail_close ? -1 : 0; }
static void trace_mkdir(const char *path, int native, int lookup) {
    assert(strncmp(path, H2_JIELI_SD_ROOT, strlen(H2_JIELI_SD_ROOT)) == 0);
    (void)native; (void)lookup; ++traces;
}
static void (*h2_jieli_sd_fs_trace_mkdir)(const char *, int, int) = trace_mkdir;
static void reset(void) {
    count = creates = closes = traces = 0;
    fail_create = fail_attr = fail_close = wrong_type = disappear = concurrent = 0;
}
'''
        main = r'''
int main(void) {
    reset();
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/long-parent/child") == H2_PAL_ERR_NOT_FOUND);
    assert(creates == 0 && traces == 0 && count == 0);
    add(H2_JIELI_SD_ROOT "data", F_ATTR_DIR);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/long-parent/child") == H2_PAL_ERR_NOT_FOUND);
    assert(creates == 0 && traces == 0 && count == 1);
    add(H2_JIELI_SD_ROOT "data/long-parent", F_ATTR_DIR);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/long-parent/child") == 0);
    assert(creates == 1 && traces == 1 && closes == 3);
    assert(strcmp(created_paths[0], H2_JIELI_SD_ROOT "data/long-parent/child") == 0);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/long-parent/child") == 0);
    assert(creates == 1 && traces == 1);

    reset();
    add(H2_JIELI_SD_ROOT "data", F_ATTR_DIR);
    add(H2_JIELI_SD_ROOT "data/long-parent", F_ATTR_DIR);
    fail_create = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/long-parent/child") == H2_PAL_ERR_IO);
    assert(creates == 1 && count == 2);
    reset();
    /* Board initialization creates these directly under the mounted root. */
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == 0);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == 0);
    assert(creates == 2 && count == 2);
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == 0);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == 0);
    assert(creates == 2);
    reset(); add(H2_JIELI_SD_ROOT "data", 0);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/child") == H2_PAL_ERR_INVALID_STATE);
    assert(creates == 0);
    reset(); add(H2_JIELI_SD_ROOT "data", F_ATTR_DIR);
    add(H2_JIELI_SD_ROOT "data/file", 0);
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/file") == H2_PAL_ERR_INVALID_STATE);
    assert(creates == 0);

    reset(); wrong_type = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == H2_PAL_ERR_INVALID_STATE);
    reset(); disappear = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == H2_PAL_ERR_IO);
    reset(); fail_attr = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == H2_PAL_ERR_IO);
    assert(closes == 2);
    reset(); add(H2_JIELI_SD_ROOT "data", F_ATTR_DIR); fail_attr = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data/child") == H2_PAL_ERR_IO);
    assert(creates == 0 && closes == 1);
    reset(); fail_close = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == H2_PAL_ERR_IO);
    reset(); concurrent = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "data") == 0);
    reset();
    assert(ensure_directory("outside/data") == H2_PAL_ERR_INVALID_ARG);
    assert(ensure_directory(H2_JIELI_SD_ROOT) == H2_PAL_ERR_INVALID_ARG);
    char longest[H2_JIELI_SD_PATH_MAX + 1];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest, H2_JIELI_SD_ROOT, strlen(H2_JIELI_SD_ROOT));
    longest[H2_JIELI_SD_PATH_MAX - 1] = 0;
    assert(ensure_directory(longest) == 0);
    longest[H2_JIELI_SD_PATH_MAX - 1] = 'a';
    longest[H2_JIELI_SD_PATH_MAX] = 0;
    assert(ensure_directory(longest) == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-directory-") as directory:
            root = Path(directory)
            unit = root / "directory.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "directory-test"
            subprocess.run(["cc", "-std=c11", str(unit), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
