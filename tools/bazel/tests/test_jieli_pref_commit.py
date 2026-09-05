"""Compile the board's actual preference commit path against injected I/O failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_pref.c"


class PreferenceCommitTest(unittest.TestCase):
    def test_replace_keeps_old_record_until_rename(self):
        source = SOURCE.read_text()
        types = source[source.index("enum {"):source.index("struct h2_pal_pref_cursor")]
        paths = source[source.index("static size_t bounded_length("):
                       source.index("static jieli_pref_namespace_t *to_namespace(")]
        write = source[source.index("static int write_record("):
                       source.index("static int pref_close(")]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int h2_pal_pref_namespace_t;
typedef int h2_pal_pref_open_mode_t;
typedef int h2_pal_pref_entry_type_t;
typedef int lfs_file_t;
typedef int lfs_ssize_t;
enum { H2_PAL_PREF_OPEN_READ_WRITE=1, H2_PAL_OK=0,
       H2_PAL_ERR_INVALID_ARG=-1, H2_PAL_ERR_INVALID_STATE=-2,
       H2_PAL_ERR_IO=-3, LFS_O_WRONLY=1, LFS_O_CREAT=2, LFS_O_TRUNC=4 };
static int pref_lfs, locked, failure, writes, synced, closed, renamed;
static int old_value, old_removes, temp_exists;
static int pref_lock(void) { assert(!locked); locked=1; return 0; }
static void pref_unlock(void) { assert(locked); locked=0; }
static int pref_prepare_locked(void) { assert(locked); return 0; }
static int map_lfs_error(int rc) { return rc < 0 ? H2_PAL_ERR_IO : 0; }
static int is_temp(const char *path) {
  size_t length=strlen(path);
  return length>=4 && strcmp(path+length-4,".tmp")==0;
}
static int lfs_file_open(int *fs, lfs_file_t *file, const char *path, int flags) {
  (void)fs; (void)file;
  assert(locked && is_temp(path) && flags==7);
  if (failure==1) return -1;
  temp_exists=1; return 0;
}
static lfs_ssize_t lfs_file_write(int *fs, lfs_file_t *file, const void *p, size_t n) {
  (void)fs; (void)file; (void)p;
  assert(locked && temp_exists && !closed);
  ++writes;
  return failure==writes+1 ? -1 : (lfs_ssize_t)n;
}
static int lfs_file_sync(int *fs, lfs_file_t *file) {
  (void)fs; (void)file;
  assert(locked && writes==3); synced=1;
  return failure==5 ? -1 : 0;
}
static int lfs_file_close(int *fs, lfs_file_t *file) {
  (void)fs; (void)file;
  assert(locked && !closed); closed=1;
  return failure==6 ? -1 : 0;
}
static int lfs_remove(int *fs, const char *path) {
  (void)fs; assert(locked);
  if (is_temp(path)) temp_exists=0;
  else { ++old_removes; old_value=0; }
  return 0;
}
static int lfs_rename(int *fs, const char *oldpath, const char *newpath) {
  (void)fs;
  assert(locked && synced && closed && temp_exists);
  assert(is_temp(oldpath) && !is_temp(newpath));
  ++renamed;
  if (failure==7) return -1;
  old_value=99; temp_exists=0; return 0;
}
'''
        main = r'''
int main(void) {
  jieli_pref_namespace_t ns = {.path="/abcd", .mode=H2_PAL_PREF_OPEN_READ_WRITE};
  const char value[]="new value";
  for (failure=0; failure<=7; ++failure) {
    old_value=42; old_removes=temp_exists=locked=writes=synced=closed=renamed=0;
    int rc=write_record(&ns,"key",1,value,sizeof(value));
    assert(rc==(failure==0 ? H2_PAL_OK : H2_PAL_ERR_IO));
    assert(!locked && old_removes==0);
    assert(old_value==(failure==0 ? 99 : 42));
    assert(renamed==((failure==0 || failure==7) ? 1 : 0));
    assert(closed==(failure==1 ? 0 : 1));
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-pref-commit-") as directory:
            test = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            test.write_text(stub + types + paths + write + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
