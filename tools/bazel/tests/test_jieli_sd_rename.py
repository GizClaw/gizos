"""Exercise real SD rename code against consuming JLFAT handles."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c"


class SdRenameTest(unittest.TestCase):
    def test_replace_and_errors(self):
        source = SOURCE.read_text()
        def section(start, end):
            return source[source.index(start):source.index(end, source.index(start))]

        fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "h2/pal/os/h2_pal_fs.h"
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define F_ATTR_DIR 16
typedef struct { int entry; } FILE;
static int present[3], attributes[3], handles, deletes, renames, closes;
static int fail_delete, fail_rename, fail_attr, fail_close_at, fail_verify;
static const char *paths[] = {H2_JIELI_SD_ROOT "data/a", H2_JIELI_SD_ROOT "data/b", H2_JIELI_SD_ROOT "data/d"};
static FILE *fopen(const char *path, const char *mode) {
  assert(strcmp(mode, "r") == 0);
  for (int i = 0; i < 3; ++i) {
    if (strcmp(path, paths[i]) != 0 || !present[i]) continue;
    if (fail_verify && renames) return NULL;
    FILE *f = malloc(sizeof(*f)); assert(f); f->entry = i; ++handles; return f;
  }
  return NULL;
}
static int fclose(FILE *f) {
  assert(f && handles > 0); free(f); --handles; ++closes;
  return closes == fail_close_at ? -1 : 0;
}
static int fget_attr(FILE *f, int *attr) { *attr = attributes[f->entry]; return fail_attr ? -1 : 0; }
static int fdelete(FILE *f) {
  ++deletes;
  assert(renames == 0 && f->entry == 1);
  if (!fail_delete) present[f->entry] = 0;
  int rc = fclose(f);
  return fail_delete ? -1 : rc;
}
static int frename(FILE *f, const char *name) {
  ++renames; assert(f->entry == 0); assert(strcmp(name, "b") == 0);
  if (fail_rename || present[1]) return -1;
  present[0] = 0; present[1] = 1; f->entry = 1; return 0;
}
#define f_free_cache(path) (0)
#define h2_jieli_atomic_cas_u32(p, e, v) __atomic_compare_exchange_n(p, e, v, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
#define h2_jieli_atomic_store_u32(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define os_time_dly(t) ((void)(t))
static void reset(void) {
  assert(handles == 0);
  memset(present, 0, sizeof(present)); memset(attributes, 0, sizeof(attributes));
  deletes = renames = closes = 0;
  fail_delete = fail_rename = fail_attr = fail_close_at = fail_verify = 0;
}
'''
        code = section("struct h2_pal_fs_file {", "static int sd_mounted;")
        code += section("static int map_error(", "/* SDK fopen already")
        code += section("static int fs_rename(", "static int wait_sd_online(")
        main = r'''
int main(void) {
  reset(); present[0] = present[1] = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_OK);
  assert(deletes == 1 && renames == 1 && !present[0] && present[1] && handles == 0);
  reset(); present[0] = 1;
  assert(fs_rename(NULL, "/data/a", "/data/a") == H2_PAL_OK);
  assert(!deletes && !renames && closes == 1);
  reset();
  assert(fs_rename(NULL, "/data/a", "/data/a") == H2_PAL_ERR_NOT_FOUND);
  reset(); present[0] = 1; fail_close_at = 1;
  assert(fs_rename(NULL, "/data/a", "/data/a") == H2_PAL_ERR_IO);
  reset(); present[0] = present[2] = 1; attributes[2] = F_ATTR_DIR;
  assert(fs_rename(NULL, "/data/a", "/data/d") == H2_PAL_ERR_INVALID_STATE);
  assert(!deletes && !renames && closes == 2);
  reset(); present[0] = 1;
  assert(fs_rename(NULL, "/data/a", "/dl/a") == H2_PAL_ERR_UNSUPPORTED);
  assert(!closes && !deletes && !renames);
  reset(); present[1] = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_NOT_FOUND);
  assert(present[1] && !deletes && !renames);
  reset(); present[0] = present[1] = 1; fail_delete = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_IO);
  assert(deletes == 1 && !renames && closes == 2);
  reset(); present[0] = present[1] = 1; fail_attr = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_IO);
  assert(!deletes && !renames && closes == 2);
  reset(); present[0] = present[2] = 1; attributes[2] = F_ATTR_DIR; fail_close_at = 1;
  assert(fs_rename(NULL, "/data/a", "/data/d") == H2_PAL_ERR_IO);
  assert(!deletes && !renames && closes == 2);
  reset(); present[0] = 1; fail_rename = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_IO);
  assert(present[0] && closes == 1);
  for (int close_at = 1; close_at <= 2; ++close_at) {
    reset(); present[0] = 1; fail_close_at = close_at;
    assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_IO);
    assert(!handles);
  }
  reset(); present[0] = 1; fail_verify = 1;
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_IO);
  assert(!handles);
  assert(fs_rename(NULL, NULL, "/data/b") == H2_PAL_ERR_INVALID_ARG);
  assert(fs_rename(NULL, "/data/a", "/other/b") == H2_PAL_ERR_INVALID_ARG);
  (void)fget_attr; (void)fdelete; (void)map_error;
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-rename-") as directory:
            unit = Path(directory) / "rename.c"
            unit.write_text(fixture + code + main)
            binary = Path(directory) / "rename-test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")),
                            "-I", str(ROOT / "libs/pal/include"), str(unit), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
