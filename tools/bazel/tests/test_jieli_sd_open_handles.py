"""Run SD handle ownership and concurrent mutation checks with fake JLFAT."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c"


class SdOpenHandlesTest(unittest.TestCase):
    def test_registry_and_concurrent_mutations(self):
        source = SOURCE.read_text()
        def section(start, end):
            return source[source.index(start):source.index(end, source.index(start))]

        fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "h2/pal/os/h2_pal_fs.h"
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define F_ATTR_DIR 16
#define h2_jieli_atomic_cas_u32(p, e, v) __atomic_compare_exchange_n(p, e, v, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
#define h2_jieli_atomic_store_u32(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define os_time_dly(t) ((void)(t), (void)sched_yield())
typedef struct { int entry; } FILE;
static const char *paths[] = {H2_JIELI_SD_ROOT "data/a", H2_JIELI_SD_ROOT "data/b",
  H2_JIELI_SD_ROOT "data/d", H2_JIELI_SD_ROOT "data/d/x", H2_JIELI_SD_ROOT "data/different", H2_JIELI_SD_ROOT "dl/H2STAGE.TMP"};
static int present[6], attributes[6], active[6], opens, closes, renames, deletes;
static int fail_attr, fail_close, reject_dir_write, fail_write, fail_delete;
static pthread_mutex_t sdk_gate = PTHREAD_MUTEX_INITIALIZER;
static const char *sd_last_stage;
static void (*h2_jieli_sd_fs_trace_write)(const char *, size_t, size_t, int);
static void sdk_lock(void) { assert(pthread_mutex_lock(&sdk_gate) == 0); }
static void sdk_unlock(void) { assert(pthread_mutex_unlock(&sdk_gate) == 0); }
static FILE *fopen(const char *path, const char *mode) {
  sdk_lock(); ++opens;
  for (int i = 0; i < 6; ++i) {
    if (strcmp(path, paths[i]) != 0) continue;
    if (strcmp(mode, "w+") == 0) {
      if (fail_write || (reject_dir_write && (attributes[i] & F_ATTR_DIR))) break;
      /* Truncation must never touch another native handle's cluster chain. */
      assert(active[i] == 0); present[i] = 1;
    } else { assert(strcmp(mode, "r") == 0); if (!present[i]) break; }
    FILE *f = malloc(sizeof(*f)); assert(f); f->entry = i; ++active[i];
    sdk_unlock(); return f;
  }
  sdk_unlock(); return NULL;
}
static int fclose(FILE *f) {
  sdk_lock(); assert(f && active[f->entry] > 0); --active[f->entry];
  free(f); ++closes; int rc = fail_close ? -1 : 0; sdk_unlock(); return rc;
}
static int fget_attr(FILE *f, int *attr) {
  sdk_lock(); *attr = attributes[f->entry]; int rc = fail_attr ? -1 : 0; sdk_unlock(); return rc;
}
static int fdelete(FILE *f) {
  sdk_lock(); assert(active[f->entry] == 1); ++deletes;
  int rc = fail_delete ? -1 : 0;
  if (!rc) present[f->entry] = 0;
  --active[f->entry]; free(f); ++closes; sdk_unlock(); return rc;
}
static int frename(FILE *f, const char *name) {
  sdk_lock(); ++renames;
  int dest = strcmp(name, "a") == 0 ? 0 : 1;
  assert(active[f->entry] == 1 && active[dest] == 0);
  if (present[dest]) { sdk_unlock(); return -1; }
  present[f->entry] = 0; --active[f->entry];
  present[dest] = 1; ++active[dest]; f->entry = dest;
  sdk_unlock(); return 0;
}
static int fdelete_dir(const char *path) {
  sdk_lock(); ++deletes;
  size_t n = strlen(path);
  for (int i = 0; i < 6; ++i) {
    if (strcmp(paths[i], path) == 0 || (strncmp(paths[i], path, n) == 0 && paths[i][n] == '/')) {
      assert(!active[i]); present[i] = 0;
    }
  }
  sdk_unlock(); return 0;
}
static int ensure_directory(const char *path) {
  sdk_lock();
  for (int i = 0; i < 6; ++i) if (strcmp(paths[i], path) == 0) {
    present[i] = 1; attributes[i] = F_ATTR_DIR;
  }
  sdk_unlock(); return 0;
}
#define f_free_cache(path) (0)
static void reset(void) {
  for (int i = 0; i < 6; ++i) assert(!active[i]);
  memset(present, 0, sizeof(present)); memset(attributes, 0, sizeof(attributes));
  opens = closes = deletes = renames = 0;
  fail_attr = fail_close = reject_dir_write = fail_write = fail_delete = 0;
}
'''
        code = section("struct h2_pal_fs_file {", "static int sd_mounted;")
        code += section("static int map_error(", "/* SDK fopen already")
        code += section("static int fs_open(", "static int fs_read(")
        code += section("static int fs_close(", "static int fs_stat(")
        code += section("static int fs_clear(", "static int wait_sd_online(")
        main = r'''
static void *open_worker(void *unused) {
  (void)unused;
  for (int i = 0; i < 3000; ++i) {
    h2_pal_fs_file_t *file = NULL;
    int rc = fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_BUSY);
    if (file) { sched_yield(); assert(fs_close(NULL, file) == H2_PAL_OK); }
  }
  return NULL;
}
static void *rename_worker(void *unused) {
  (void)unused;
  for (int i = 0; i < 3000; ++i) {
    int rc = fs_rename(NULL, "/data/a", "/data/b");
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_BUSY || rc == H2_PAL_ERR_NOT_FOUND);
    rc = fs_rename(NULL, "/data/b", "/data/a");
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_BUSY || rc == H2_PAL_ERR_NOT_FOUND);
  }
  return NULL;
}
int main(void) {
  (void)fget_attr;
  h2_pal_fs_file_t *a = NULL, *b = NULL;
  reset(); present[2] = 1; attributes[2] = F_ATTR_DIR;
  assert(fs_open(NULL, "/data/d", H2_PAL_FS_OPEN_READ, &a) == H2_PAL_ERR_INVALID_STATE);
  assert(!a && active[2] == 0 && closes == 1);
  assert(fs_open(NULL, "/data/d", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_ERR_INVALID_STATE);
  assert(!a && !active[2]);
  reject_dir_write = 1;
  assert(fs_open(NULL, "/data/d", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_ERR_INVALID_STATE);
  assert(!a && !active[2]);
  reset();
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_OK);
  int before = opens;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &b) == H2_PAL_ERR_BUSY && !b);
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_READ, &b) == H2_PAL_ERR_BUSY && !b);
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_ERR_BUSY);
  assert(fs_rename(NULL, "/data/b", "/data/a") == H2_PAL_ERR_BUSY);
  assert(fs_remove(NULL, "/data/a") == H2_PAL_ERR_BUSY);
  assert(fs_clear(NULL, "/data/a") == H2_PAL_ERR_BUSY);
  assert(fs_clear(NULL, "/data") == H2_PAL_ERR_BUSY);
  assert(opens == before && !deletes && !renames);
  assert(fs_close(NULL, a) == H2_PAL_OK);
  assert(fs_rename(NULL, "/data/a", "/data/b") == H2_PAL_OK);
  assert(fs_remove(NULL, "/data/b") == H2_PAL_OK);
  assert(fs_clear(NULL, "/data") == H2_PAL_OK);
  reset(); present[0] = 1;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_READ, &a) == H2_PAL_OK);
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_READ, &b) == H2_PAL_OK);
  h2_pal_fs_file_t *writer = NULL;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &writer) == H2_PAL_ERR_BUSY);
  assert(fs_close(NULL, a) == H2_PAL_OK);
  assert(fs_remove(NULL, "/data/a") == H2_PAL_ERR_BUSY);
  assert(fs_close(NULL, b) == H2_PAL_OK);
  assert(fs_remove(NULL, "/data/a") == H2_PAL_OK);
  reset();
  assert(fs_open(NULL, "/data/d/x", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_OK);
  assert(fs_clear(NULL, "/data/d") == H2_PAL_ERR_BUSY);
  assert(fs_close(NULL, a) == H2_PAL_OK);
  assert(fs_clear(NULL, "/data/d") == H2_PAL_OK);
  assert(fs_open(NULL, "/data/different", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_OK);
  assert(fs_clear(NULL, "/data/d") == H2_PAL_OK);
  assert(fs_close(NULL, a) == H2_PAL_OK);
  reset();
  assert(fs_open(NULL, "/dl/update.tar.zlib.tmp", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_OK);
  assert(fs_open(NULL, "/dl/H2STAGE.TMP", H2_PAL_FS_OPEN_READ, &b) == H2_PAL_ERR_BUSY);
  assert(fs_remove(NULL, "/dl/H2STAGE.TMP") == H2_PAL_ERR_BUSY);
  assert(fs_clear(NULL, "/dl") == H2_PAL_ERR_BUSY);
  assert(fs_close(NULL, a) == H2_PAL_OK);
  assert(fs_remove(NULL, "/dl/H2STAGE.TMP") == H2_PAL_OK);
  reset(); fail_attr = 1;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_ERR_IO);
  assert(!a && !active[0]); fail_attr = 0;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_OK);
  fail_close = 1;
  assert(fs_close(NULL, a) == H2_PAL_ERR_IO); fail_close = 0;
  assert(fs_remove(NULL, "/data/a") == H2_PAL_OK);
  reset(); present[2] = 1; attributes[2] = F_ATTR_DIR; fail_close = 1;
  assert(fs_open(NULL, "/data/d", H2_PAL_FS_OPEN_READ, &a) == H2_PAL_ERR_IO);
  assert(!a && !active[2]);
  reset(); fail_write = 1;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &a) == H2_PAL_ERR_IO);
  fail_write = 0;
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_READ, &a) == H2_PAL_ERR_NOT_FOUND);
  assert(fs_open(NULL, "/data/a", 99, &a) == H2_PAL_ERR_INVALID_ARG);
  assert(fs_open(NULL, NULL, H2_PAL_FS_OPEN_READ, &a) == H2_PAL_ERR_INVALID_ARG);
  assert(fs_open(NULL, "/data/a", H2_PAL_FS_OPEN_READ, NULL) == H2_PAL_ERR_INVALID_ARG);
  reset();
  pthread_t opener, opener2, renamer;
  assert(pthread_create(&opener, NULL, open_worker, NULL) == 0);
  assert(pthread_create(&opener2, NULL, open_worker, NULL) == 0);
  assert(pthread_create(&renamer, NULL, rename_worker, NULL) == 0);
  assert(pthread_join(opener, NULL) == 0);
  assert(pthread_join(opener2, NULL) == 0);
  assert(pthread_join(renamer, NULL) == 0);
  reset();
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-open-") as directory:
            unit = Path(directory) / "open.c"
            unit.write_text(fixture + code + main)
            binary = Path(directory) / "open-test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                            *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")),
                            "-I", str(ROOT / "libs/pal/include"), str(unit), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
